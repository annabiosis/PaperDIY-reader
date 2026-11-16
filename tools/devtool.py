#!/usr/bin/env python3
"""Developer tool for the e-reader firmware over USB-Serial-JTAG.

Commands:
  ping                        check the console
  shot <out.png>              grab a grayscale screenshot of the panel buffer
  put <local> <remote>        upload a file to the SD card
  key A|B|AB|UP|DN            inject a button gesture
  open <remote-path>          open a file directly
  reset                       forget the saved position
  demo                        upload the bundled demo books and open the first
"""

from __future__ import annotations

import argparse
import base64
import io
import os
import sys
import time

import serial

try:
    from PIL import Image
except ImportError:  # screenshots need Pillow only
    Image = None

DEFAULT_PORT = "COM5"

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # older interpreters
    pass


class Console:
    def __init__(self, port: str, baud: int = 115200, settle: float = 2.5):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.ser.setDTR(False)
        self.ser.setRTS(False)
        self.buffer = bytearray()
        # Opening the port may reset the board; give it time to boot.
        time.sleep(settle)
        self.drain(0.3)

    def close(self) -> None:
        self.ser.close()

    def wait_ready(self, timeout: float = 20.0) -> bool:
        """Waits until the firmware answers, coping with the reset on port open."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.buffer.clear()
            self.ser.write(b"rdr ping\n")
            self.ser.flush()
            end = time.time() + 0.6
            while time.time() < end:
                chunk = self.ser.read(4096)
                if chunk:
                    self.buffer += chunk
                    if b"rdr pong" in self.buffer:
                        return True
        return False

    def drain(self, seconds: float = 0.4) -> None:
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                self.buffer += chunk

    def command(self, text: str, wait: float = 1.0, attempts: int = 3) -> str:
        for attempt in range(attempts):
            self.buffer.clear()
            self.ser.write((f"rdr {text}\n").encode())
            self.ser.flush()
            end = time.time() + wait
            while time.time() < end:
                chunk = self.ser.read(4096)
                if chunk:
                    self.buffer += chunk
                    if b"rdr ok" in self.buffer or b"rdr pong" in self.buffer or b"rdr err" in self.buffer:
                        break
            if b"rdr " in self.buffer:
                break
            time.sleep(0.3)
        return self.buffer.decode("utf-8", "replace")

    def screenshot(self, path: str, region: tuple[int, int, int, int] | None = None) -> None:
        self.buffer.clear()
        if region:
            self.ser.write(f"rdr shot {region[0]} {region[1]} {region[2]} {region[3]}\n".encode())
        else:
            self.ser.write(b"rdr shot\n")
        self.ser.flush()
        # Wait for the header line: "rdr shot x y w h pixels". Log lines may arrive
        # first (especially right after a reset), so skip anything that is not it.
        rest = b""
        parts: list[str] = []
        deadline = time.time() + 20
        while time.time() < deadline:
            header = b""
            while b"\n" not in header and time.time() < deadline:
                chunk = self.ser.read(256)
                if chunk:
                    header += chunk
            if not header:
                continue
            line, _, tail = header.partition(b"\n")
            parts = line.decode("utf-8", "replace").split()
            if len(parts) == 7 and parts[0] == "rdr" and parts[1] == "shot":
                rest = tail
                break
        if len(parts) != 7 or parts[1] != "shot":
            raise RuntimeError(f"unexpected screenshot header: {line!r}")
        x, y, w, h, total = (int(value) for value in parts[2:7])

        data = bytearray(rest)
        deadline = time.time() + 60
        while len(data) < total and time.time() < deadline:
            chunk = self.ser.read(min(65536, total - len(data)))
            if chunk:
                data += chunk
        if len(data) < total:
            raise RuntimeError(f"short screenshot: {len(data)}/{total}")

        self._read_line(2.0)
        image = Image.frombytes("L", (w, h), bytes(data[:total]))
        if region is None:
            image.save(path)
        else:
            print(f"saved region {x},{y} {w}x{h} -> {path}")
            image.save(path)
        print(f"saved {path} ({w}x{h})")

    def bench(self, wait: float = 90.0) -> str:
        self.buffer.clear()
        self.ser.write(b"rdr bench\n")
        self.ser.flush()
        end = time.time() + wait
        while time.time() < end:
            chunk = self.ser.read(4096)
            if chunk:
                self.buffer += chunk
                if b"rdr bench end" in self.buffer:
                    break
        return self.buffer.decode("utf-8", "replace")

    def get_text(self, remote: str) -> str:
        """Reads a small text file from the SD card (`get`, raw framing)."""
        self.buffer.clear()
        self.ser.write(f"rdr get {remote}\n".encode())
        self.ser.flush()
        header = self.ser.readline()
        parts = header.decode("utf-8", "replace").split()
        if len(parts) != 3 or parts[1] != "raw":
            raise RuntimeError(f"unexpected get header: {header!r}")
        total = int(parts[2])
        data = bytearray()
        deadline = time.time() + 30
        while len(data) < total and time.time() < deadline:
            chunk = self.ser.read(min(4096, total - len(data)))
            if chunk:
                data += chunk
        self._read_line(2.0)
        return bytes(data[:total]).decode("utf-8", "replace")

    def fetch_file(self, remote: str, local: str, timeout: float = 240.0) -> int:
        """Downloads a file from the SD card (`getb64`). Returns bytes written."""
        self.buffer.clear()
        self.ser.write(f"rdr getb64 {remote}\n".encode())
        self.ser.flush()
        header = self.ser.readline()
        if not header.startswith(b"rdr b64"):
            raise RuntimeError(f"unexpected download header: {header!r}")
        payload = bytearray()
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(65536)
            if not chunk:
                continue
            end = chunk.find(b"rdr end")
            if end >= 0:
                payload += chunk[:end]
                break
            payload += chunk
        data = base64.b64decode(bytes(payload))
        with open(local, "wb") as handle:
            handle.write(data)
        return len(data)

    def _read_framed(self, kind: str, header_timeout: float = 10.0, body_timeout: float = 30.0) -> str:
        """Reads `rdr <kind> <bytes>` followed by the payload and `rdr end`.

        Log lines (``I (1234) App: ...``) may arrive between the request and the
        framed reply — especially after a reset — so they are skipped.
        """
        deadline = time.time() + header_timeout
        rest = b""
        total = -1
        while time.time() < deadline:
            header = b""
            while b"\n" not in header and time.time() < deadline:
                chunk = self.ser.read(256)
                if chunk:
                    header += chunk
            if not header:
                continue
            line, _, tail = header.partition(b"\n")
            parts = line.decode("utf-8", "replace").split()
            if len(parts) == 3 and parts[0] == "rdr" and parts[1] == kind:
                total = int(parts[2])
                rest = tail
                break
            # Anything else is a log line or an unrelated reply: keep waiting.
        if total < 0:
            raise RuntimeError(f"no framed {kind} reply from the device")
        data = bytearray(rest)
        deadline = time.time() + body_timeout
        while len(data) < total and time.time() < deadline:
            chunk = self.ser.read(min(4096, total - len(data)))
            if chunk:
                data += chunk
        # Consume the trailing "rdr end" line so the next command parses cleanly.
        self._read_line(2.0)
        return bytes(data[:total]).decode("utf-8", "replace")

    def _read_line(self, timeout: float) -> bytes:
        line = b""
        deadline = time.time() + timeout
        while b"\n" not in line and time.time() < deadline:
            chunk = self.ser.read(64)
            if chunk:
                line += chunk
        return line

    def dump(self) -> str:
        self.buffer.clear()
        self.ser.write(b"rdr dump\n")
        self.ser.flush()
        return self._read_framed("dump", body_timeout=20)

    def put(self, local: str, remote: str) -> None:
        with open(local, "rb") as handle:
            payload = base64.b64encode(handle.read()).decode("ascii")
        reply = self.command(f"put {remote} {payload}", wait=20)
        print(reply.strip().splitlines()[-1] if reply.strip() else "")

    def key(self, name: str) -> None:
        print(self.command(f"key {name}", wait=2).strip())

    def open(self, remote: str) -> None:
        print(self.command(f"open {remote}", wait=10).strip())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default=os.environ.get("READER_PORT", DEFAULT_PORT))
    parser.add_argument("command")
    parser.add_argument("args", nargs="*")
    options = parser.parse_args()

    console = Console(options.port)
    try:
        if not console.wait_ready():
            print("device did not answer (check the port and that the board is running)", file=sys.stderr)
            return 3
        if options.command == "ping":
            print(console.command("ping").strip())
        elif options.command == "shot":
            args = options.args
            name = args[0] if args else "shot.png"
            region = tuple(int(v) for v in args[1:5]) if len(args) >= 5 else None
            console.screenshot(name, region)
        elif options.command == "put":
            console.put(options.args[0], options.args[1])
        elif options.command == "key":
            console.key(options.args[0])
        elif options.command == "open":
            console.open(options.args[0])
        elif options.command == "dump":
            print(console.dump())
        elif options.command == "reset":
            print(console.command("reset", wait=3).strip())
        elif options.command == "session":
            # Several commands over one connection: opening the port resets the
            # board, so state must be built up within a single session.
            for step in options.args:
                verb, _, argument = step.partition(":")
                if verb == "open":
                    console.open(argument)
                elif verb == "dump":
                    print(console.dump())
                elif verb == "shot":
                    console.screenshot(argument)
                elif verb == "key":
                    console.key(argument)
                elif verb == "put":
                    local, _, remote = argument.partition(":")
                    console.put(local, remote)
                elif verb == "reboot":
                    print(console.command("reboot", wait=2).strip())
                elif verb == "reset":
                    print(console.command("reset", wait=3).strip())
                elif verb == "openb64":
                    print(console.command(f"openb64 {argument}", wait=20).strip())
                elif verb == "probe":
                    print(console.command(f"probe {argument}", wait=5).strip())
                elif verb == "get":
                    print(console.get_text(argument))
                elif verb == "download":
                    remote, _, local = argument.partition(":")
                    size = console.fetch_file(remote, local)
                    print(f"downloaded {local} ({size} bytes)")
                elif verb == "burst":
                    print(console.command(f"burst {argument}", wait=20).strip())
                elif verb == "bench":
                    print(console.bench(wait=90))
                elif verb == "rm":
                    print(console.command(f"rm {argument}", wait=8).strip())
                elif verb == "heal":
                    print(console.command(f"heal {argument}", wait=20).strip())
                elif verb == "ls":
                    print(console.command(f"ls {argument}", wait=8))
                elif verb == "limit":
                    print(console.command(f"limit {argument}", wait=4).strip())
                elif verb == "stats":
                    print(console.command("stats", wait=3).strip())
                elif verb == "ping":
                    print(console.command("ping").strip())
                elif verb == "sleep":
                    time.sleep(float(argument or 1))
                else:
                    print(f"unknown step: {step}", file=sys.stderr)
                    return 2
        elif options.command == "demo":
            here = os.path.dirname(os.path.abspath(__file__))
            demo_dir = os.path.join(here, "..", "demo")
            for name in sorted(os.listdir(demo_dir)):
                source = os.path.join(demo_dir, name)
                if os.path.isfile(source):
                    console.put(source, name)
            console.command("reset", wait=3)
            console.open("demo.md")
        else:
            print(f"unknown command: {options.command}", file=sys.stderr)
            return 2
    finally:
        console.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
