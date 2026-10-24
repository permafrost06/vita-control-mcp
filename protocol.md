# VC01 wire protocol

TCP, default port 5000. All multibyte integers use **big endian**, independent
of the Vita's native byte order. One outstanding request per connection.

Requests start with 12 bytes: magic ASCII `VC01`, opcode `u32`, payload length
`u32`, followed by exactly that many payload bytes. Request payloads are limited
to 128 bytes. Bad magic or a truncated frame closes the connection. Oversized
requests receive status 1, then the connection closes without reading their body.

Responses have the same header, with opcode ORed with `0x80000000`. The response
payload always starts with a status `u32`, included in its length. The remaining
bytes are the operation's result. Maximum response payload length is
`4 + 8 + 960 * 544 * 3` bytes. Error replies have no result body.

Status codes: 0 success, 1 invalid request, 2 unauthorized, 3 no new frame
within two seconds, 4 unsupported framebuffer.

## Operations

1. **AUTH**: first request must contain the raw shared token, 32–64 printable
   ASCII bytes without whitespace or a newline. Success has an empty result.
   Authentication failure closes the connection after its response. File-ending
   CR/LF characters are stripped only when loading the token from disk.
2. **INFO**: empty request. Success returns four `u32` values: protocol version
   (1), controller hook count, display hook present (0/1), maximum duration in
   milliseconds (5000). This is import availability, not runtime compatibility.
3. **SCREENSHOT**: empty request. Success returns width `u32`, height `u32`,
   then `width * height * 3` RGB bytes, top-left first, row-major, without pitch
   padding. Capture occurs on the next supported main-module frame submission.
4. **INPUT**: exactly 16 payload bytes: buttons `u32`, duration ms `u32`,
   `lx u8`, `ly u8`, `rx u8`, `ry u8`, flags `u32`. Flag bit 0 enables the
   left-stick override, bit 1 enables the right-stick override. All other bits
   must be zero. Axes range from 0 to 255, center 128; positive y is down.
   Durations must be 1–5000. Success has an empty result and acknowledges the
   installation of state, **not** completion of the hold. Keep the connection
   open for the desired duration. A new INPUT replaces the previous injected
   state; it does not queue. Unspecified sticks preserve physical input.
5. **RELEASE**: empty request and result. Immediately clears injected state in
   this session. On disconnect or socket error the server also clears it.

Button bits: select `0x0001`, start `0x0008`, up `0x0010`, right `0x0020`,
down `0x0040`, left `0x0080`, L `0x0100`, R `0x0200`, triangle `0x1000`,
circle `0x2000`, cross `0x4000`, square `0x8000`. Other bits are rejected.
L/R are translated to L1/R1 for controller APIs that use the alternate mapping.
Positive-logic reads OR injected bits; negative-logic reads clear those bits.
Only controller port 0 is modified. Every successfully returned sample in a
buffer is updated while a timed action is active.

The server serves one client at a time. Its per-socket send/receive idle timeout
is 15 seconds. The desktop bridge uses a ten-second response deadline, opens
one connection per tool, authenticates, performs the operation, then closes.
For INPUT it waits for the requested duration and issues RELEASE before closing.
No mutating request is retried automatically.
