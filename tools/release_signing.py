"""Release signature helpers. Hosted keys travel only through signer stdin."""
import argparse
from pathlib import Path
import subprocess
import sys

SIGNING_SCRIPT = Path(__file__).with_name("release-signing.ps1")


def add_key_arguments(parser: argparse.ArgumentParser) -> None:
    keys = parser.add_mutually_exclusive_group()
    keys.add_argument("--key-path", type=Path)
    keys.add_argument("--signing-key-stdin", action="store_true",
                      help="Read the hosted signing key from standard input")


def read_signing_key(from_stdin: bool) -> bytes | None:
    if not from_stdin:
        return None
    line = sys.stdin.buffer.readline(1025)
    if not line.endswith(b"\n") or len(line) > 1024 or not line.strip():
        raise ValueError("Hosted signing key input is missing or invalid")
    return line


def signature(command: str, path: Path, key_path: Path | None = None,
              hosted_key: bytes | None = None) -> None:
    args = ["pwsh", "-NoProfile", "-File", str(SIGNING_SCRIPT),
            command, "-InputFile", str(path)]
    if command == "sign":
        if hosted_key is not None:
            args.append("-ReadKeyFromStdin")
        elif key_path is not None:
            args.extend(("-KeyPath", str(key_path)))
    subprocess.run(args, input=hosted_key if command == "sign" else None,
                   check=True)
