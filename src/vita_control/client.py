"""VC01 transport. No MCP dependency is needed to use this module."""

from __future__ import annotations

import math
import os
import socket
import struct
import time
import zlib
from pathlib import Path

MAGIC = b"VC01"
HEADER = struct.Struct("!4sII")
AUTH, INFO, SCREENSHOT, INPUT, RELEASE = range(1, 6)
MAX_RGB = 960 * 544 * 3
MAX_RESPONSE = MAX_RGB + 12
BUTTONS = {
    "select": 0x0001,
    "start": 0x0008,
    "up": 0x0010,
    "right": 0x0020,
    "down": 0x0040,
    "left": 0x0080,
    "l": 0x0100,
    "r": 0x0200,
    "triangle": 0x1000,
    "circle": 0x2000,
    "cross": 0x4000,
    "square": 0x8000,
}
ERRORS = {
    1: "Invalid request or input range",
    2: "Authentication failed: check the Vita and host tokens",
    3: "No submitted frame within 2 seconds; resume the game and try again",
    4: "Unsupported framebuffer: requires RGBA8 at up to 960x544",
}


class VitaError(RuntimeError):
    """Device, connection, or wire protocol error."""


def validate_token(token: str) -> bytes:
    try:
        encoded = token.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError("Token must contain printable ASCII characters") from exc
    if not 32 <= len(encoded) <= 64 or any(c < 33 or c > 126 for c in encoded):
        raise ValueError("Token must contain 32-64 printable ASCII characters without spaces")
    return encoded


def settings() -> tuple[str, int, str]:
    host = os.environ.get("VITA_HOST", "").strip()
    if not host:
        raise ValueError("Set VITA_HOST to the Vita's Wi-Fi IP address")
    port = int(os.environ.get("VITA_PORT", "5000"))
    if not 1 <= port <= 65535:
        raise ValueError("VITA_PORT must be between 1 and 65535")
    token = os.environ.get("VITA_TOKEN")
    if token is None:
        token_path = os.environ.get("VITA_TOKEN_FILE")
        if not token_path:
            raise ValueError("Set VITA_TOKEN or VITA_TOKEN_FILE")
        token = Path(token_path).read_text(encoding="ascii").rstrip("\r\n")
    validate_token(token)
    return host, port, token


def encode_input(
    buttons: list[str],
    duration_ms: int,
    left_stick: tuple[float, float] | list[float] | None = None,
    right_stick: tuple[float, float] | list[float] | None = None,
) -> bytes:
    if type(duration_ms) is not int or not 1 <= duration_ms <= 5000:
        raise ValueError("duration_ms must be an integer between 1 and 5000")
    mask = 0
    for name in buttons:
        if not isinstance(name, str) or name.lower() not in BUTTONS:
            raise ValueError(f"Unknown button {name!r}; choose from {', '.join(BUTTONS)}")
        mask |= BUTTONS[name.lower()]
    axes = [128] * 4
    flags = 0
    for index, stick in enumerate((left_stick, right_stick)):
        if stick is None:
            continue
        if len(stick) != 2:
            raise ValueError("Each stick requires [x, y], with values between -1 and 1")
        for axis, value in enumerate(stick):
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                raise ValueError("Stick values must be numbers between -1 and 1")
            if not math.isfinite(value) or not -1 <= value <= 1:
                raise ValueError("Stick values must be finite and between -1 and 1")
            axes[index * 2 + axis] = round((value + 1) * 127.5)
        flags |= 1 << index
    return struct.pack("!II4BI", mask, duration_ms, *axes, flags)


def rgb_to_png(width: int, height: int, rgb: bytes) -> bytes:
    if not 1 <= width <= 960 or not 1 <= height <= 544 or len(rgb) != width * height * 3:
        raise VitaError("Invalid screenshot dimensions or pixel length")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (
            struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))
        )

    stride = width * 3
    scanlines = b"".join(b"\x00" + rgb[y * stride : (y + 1) * stride] for y in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack("!IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(scanlines, level=3))
        + chunk(b"IEND", b"")
    )


class VitaClient:
    """One authenticated session. Closing a session releases its injected input.

    Use a context manager for each operation; do not share an instance between threads.
    Mutating requests are never automatically retried.
    """

    def __init__(self, host: str, port: int, token: str, timeout: float = 10):
        self.host, self.port = host, port
        self.token = validate_token(token)
        self.timeout = timeout
        self.sock: socket.socket | None = None

    @classmethod
    def from_env(cls) -> VitaClient:
        return cls(*settings())

    def __enter__(self) -> VitaClient:
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
            self.request(AUTH, self.token)
            return self
        except OSError as exc:
            self.close()
            raise VitaError(f"Cannot connect to Vita at {self.host}:{self.port}: {exc}") from exc
        except BaseException:
            self.close()
            raise

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def _read(self, size: int, deadline: float) -> bytes:
        assert self.sock is not None
        parts = bytearray()
        while len(parts) < size:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise VitaError("Vita response timed out")
            self.sock.settimeout(remaining)
            data = self.sock.recv(size - len(parts))
            if not data:
                raise VitaError("Vita closed the connection before the response completed")
            parts.extend(data)
        return bytes(parts)

    def request(self, op: int, payload: bytes = b"") -> bytes:
        if self.sock is None:
            raise VitaError("Use VitaClient as a context manager before sending commands")
        try:
            self.sock.settimeout(self.timeout)
            self.sock.sendall(HEADER.pack(MAGIC, op, len(payload)) + payload)
            deadline = time.monotonic() + self.timeout
            magic, reply_op, size = HEADER.unpack(self._read(HEADER.size, deadline))
            if magic != MAGIC or reply_op != op | 0x80000000 or not 4 <= size <= MAX_RESPONSE:
                raise VitaError("Invalid Vita response header")
            data = self._read(size, deadline)
            status = struct.unpack_from("!I", data)[0]
            if status:
                raise VitaError(ERRORS.get(status, f"Unknown Vita error {status}"))
            return data[4:]
        except OSError as exc:
            self.close()
            raise VitaError(f"Vita transport failed: {exc}") from exc
        except VitaError:
            self.close()
            raise

    def info(self) -> dict[str, int | bool]:
        data = self.request(INFO)
        if len(data) != 16:
            raise VitaError("Invalid device info response")
        version, hooks, display, duration = struct.unpack("!4I", data)
        if version != 1:
            raise VitaError(f"Unsupported Vita protocol version {version}")
        return {
            "protocol_version": version,
            "controller_hooks": hooks,
            "screenshot_hook": bool(display),
            "max_duration_ms": duration,
        }

    def screenshot(self) -> bytes:
        data = self.request(SCREENSHOT)
        if len(data) < 8:
            raise VitaError("Truncated screenshot metadata")
        width, height = struct.unpack_from("!II", data)
        return rgb_to_png(width, height, data[8:])

    def press(
        self,
        buttons: list[str],
        duration_ms: int = 100,
        left_stick: tuple[float, float] | list[float] | None = None,
        right_stick: tuple[float, float] | list[float] | None = None,
    ) -> None:
        payload = encode_input(buttons, duration_ms, left_stick, right_stick)
        self.request(INPUT, payload)
        # Keep the connection alive during the hold; device also enforces expiry.
        try:
            time.sleep(duration_ms / 1000)
        finally:
            if self.sock is not None:
                self.release()

    def release(self) -> None:
        self.request(RELEASE)
