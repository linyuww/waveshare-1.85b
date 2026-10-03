"""Send bounded, allowlisted assistant diagnostics over the board's USB console."""
import argparse
from pathlib import Path
import sys
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--command", action="append", choices=["status", "connect", "toggle", "stop", "open", "help"], default=[])
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.seconds <= 300:
        parser.error("--seconds must be in (0, 300]")
    commands = ["help" if command == "help" else f"xiaozhi {command}" for command in args.command]
    if not commands or any(len(command.encode()) >= 320 for command in commands):
        parser.error("Provide a bounded assistant command")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    connection = serial.Serial(baudrate=115200, timeout=0.2)
    connection.dtr = False
    connection.rts = False
    connection.port = args.port
    with connection, args.output.open("wb") as output:
        for command in commands:
            connection.write((command + "\r\n").encode("utf-8"))
        connection.flush()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            data = connection.read(connection.in_waiting or 1)
            if data:
                output.write(data)
                output.flush()
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()


if __name__ == "__main__":
    main()
