"""A fake pov-strip device on a pseudo-terminal, for testing the host tool without
hardware. Speaks the same console and upload protocol as the firmware.

    povstrip serve --fake        (the server opens one for you)
    python3 -m povstrip.fakedev  (prints the pty path to use with --serial)
"""
from __future__ import annotations

import os
import threading
import zlib

from .protocol import HEADER_BYTES, CHUNK, PovHeader


class FakeDevice:
    def __init__(self):
        self.master, self.slave = os.openpty()
        self.path = os.ttyname(self.slave)
        self.store: dict[int, bytes] = {}
        self.settings: dict[str, str] = {"len": "2.00", "dens": "60", "bri": "255", "gamma": "2.40", "mode": "full",
                                         "dir": "img", "col": "0", "gap": "img", "auto": "on"}
        self.current = 1
        self.test = False
        self.commands: list[str] = []
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._write("\n-- POV strip 2.0 (fake) --\nready\n")

    def close(self):
        self._stop = True
        os.close(self.master)

    def _write(self, s: str | bytes):
        os.write(self.master, s.encode() if isinstance(s, str) else s)

    def _read_exact(self, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = os.read(self.master, n - len(buf))
            if not chunk:
                break
            buf += chunk
        return buf

    def _run(self):
        line = b""
        while not self._stop:
            try:
                b = os.read(self.master, 1)
            except OSError:
                return
            if not b:
                return
            if b in (b"\n", b"\r"):
                if line:
                    self._handle(line.decode("utf-8", "replace"))
                    line = b""
            else:
                line += b

    def _info(self):
        n = len(self.store)
        self._write(f"\n-- POV strip 2.0 (fake) --\nchip WS2812B (single-wire)\nstrip {self.settings['len']} m x "
                    f"{self.settings['dens']}/m = 120 LEDs (max 720)\nimage {self.current}/{max(1, n)}"
                    f"{'' if n else ' (compiled-in)'}: {'slot' if n else 'Heart'}, 120 rows x 16 cols\n"
                    f"column 3905 us (256 Hz); 16 columns per pass = 62.5 ms; gap 20.9 ms\n"
                    f"dir ltr (image), render {self.settings['mode']}, gamma {self.settings['gamma']}, bri {self.settings['bri']}, auto {self.settings['auto']}\n")

    def _handle(self, text: str):
        self.commands.append(text)
        parts = text.split()
        cmd, args = parts[0].lower(), parts[1:]
        if cmd == "ping":
            self._write("ok\n")
        elif cmd == "ver":
            self._write("pov 2.0 fake\nok\n")
        elif cmd in ("info", "help", "next", "save", "load", "reset"):
            if cmd == "save":
                self._write("saved\n")
            self._info()
            self._write("ok\n")
        elif cmd == "test":
            self.test = not (args and args[0] == "off")
            self._write("test frame on\nok\n" if self.test else "POV resumed\nok\n")
        elif cmd == "ls":
            self._write(f"store: {len(self.store)} images\n")
            for slot in sorted(self.store):
                h = PovHeader.unpack(self.store[slot])
                self._write(f"  {slot:2d}  {h.rows}x{h.cols}  {len(self.store[slot])} bytes  {h.name}\n")
            self._write("ok\n")
        elif cmd == "wipe":
            self.store.clear()
            self._write("store emptied\nok\n")
        elif cmd == "rm":
            slot = int(args[0]) if args else 0
            if slot in self.store:
                del self.store[slot]
                self._write("ok\n")
            else:
                self._write("err no such slot\n")
        elif cmd == "put":
            if len(args) != 3:
                self._write("err usage: put <slot> <bytes> <crc32hex>\n")
                return
            slot, total, crc = int(args[0]), int(args[1]), int(args[2], 16)
            if not 1 <= slot <= 32:
                self._write("err slot 1..32\n")
                return
            self._write("go\n")
            data = b""
            while len(data) < total:
                want = min(CHUNK, total - len(data))
                chunk = self._read_exact(want)
                if len(chunk) != want:
                    self._write("err timeout\n")
                    return
                data += chunk
                self._write(".\n")
            if (zlib.crc32(data) & 0xFFFFFFFF) != crc:
                self._write("err crc\n")
                return
            try:
                h = PovHeader.unpack(data)
            except Exception:
                self._write("err header\n")
                return
            if len(data) != HEADER_BYTES + h.rows * h.cols * 3:
                self._write("err header\n")
                return
            self.store[slot] = data
            self.current = sorted(self.store).index(slot) + 1
            self._write(f"ok {total}\n")
        elif cmd in ("len", "dens", "bri", "gamma", "mode", "dir", "col", "gap", "auto", "img", "mhz"):
            if not args:
                self._write("err missing value\n")
                return
            if cmd == "mhz":
                self._write("err mhz only applies to clocked chips\n")
                return
            self.settings[cmd] = args[0]
            self._info()
            self._write("ok\n")
        else:
            self._write("err unknown command; 'help' lists them\n")


if __name__ == "__main__":
    dev = FakeDevice()
    print(dev.path, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
