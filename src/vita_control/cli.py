"""Small manual client for installation checks without an LLM."""

import argparse
import json
import secrets
import sys
from pathlib import Path

from .client import VitaClient, VitaError


def main() -> None:
    parser = argparse.ArgumentParser(description="PS Vita screenshot and controller client")
    commands = parser.add_subparsers(dest="command", required=True)
    token = commands.add_parser("token", help="Create a new shared token file (never overwrites)")
    token.add_argument("--output", type=Path, default=Path("token.txt"))
    commands.add_parser("status")
    shot = commands.add_parser("screenshot")
    shot.add_argument("--output", type=Path, default=Path("screenshot.png"))
    press = commands.add_parser("press")
    press.add_argument("buttons", nargs="*")
    press.add_argument("--duration-ms", type=int, default=100)
    press.add_argument("--left-stick", type=float, nargs=2, metavar=("X", "Y"))
    press.add_argument("--right-stick", type=float, nargs=2, metavar=("X", "Y"))
    commands.add_parser("release")
    args = parser.parse_args()
    try:
        if args.command == "token":
            with open(args.output, "x", encoding="ascii", opener=_private_file) as out:
                out.write(secrets.token_hex(32) + "\n")
            print(f"Created {args.output}. Copy it to ux0:data/vita-control/token.txt")
            return
        with VitaClient.from_env() as client:
            if args.command == "status":
                print(json.dumps(client.info(), indent=2))
            elif args.command == "screenshot":
                args.output.write_bytes(client.screenshot())
                print(f"Saved {args.output}")
            elif args.command == "press":
                client.press(args.buttons, args.duration_ms, args.left_stick, args.right_stick)
                print("Input completed and released.")
            else:
                client.release()
                print("Input released.")
    except (VitaError, ValueError, OSError) as exc:
        print(f"vita-control: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


def _private_file(path: str, flags: int) -> int:
    import os

    return os.open(path, flags, 0o600)


if __name__ == "__main__":
    main()
