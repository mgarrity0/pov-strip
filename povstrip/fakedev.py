"""A simulated pov-strip, for using the tool without hardware. Speaks the same
console and upload protocol as the firmware, over an in-memory pipe (any OS):

    povstrip serve --fake          the server connects to it as port "fake://"
    python3 -m povstrip.fakedev    Linux/macOS only: a pseudo-terminal to use with --serial
"""
from __future__ import annotations

import os
import threading
import time
import zlib

from .protocol import HEADER_BYTES, CHUNK, PovHeader

FAKE_PORT = "fake://"


class _Pipe:
    """One direction of the in-memory link."""

    def __init__(self):
        self.buf = bytearray()
        self.cv = threading.Condition()
        self.closed = False

    def put(self, data: bytes):
        with self.cv:
            self.buf += data
            self.cv.notify_all()

    def get(self, n: int, timeout: float) -> bytes:
        with self.cv:
            if not self.buf and not self.closed:
                self.cv.wait(timeout)
            out = bytes(self.buf[:n])
            del self.buf[:n]
            return out

    def clear(self):
        with self.cv:
            self.buf.clear()

    def close(self):
        with self.cv:
            self.closed = True
            self.cv.notify_all()


class _Endpoint:
    """The host side, with the subset of the pyserial Serial API that Device uses."""

    def __init__(self, to_dev: _Pipe, from_dev: _Pipe):
        self._to, self._from = to_dev, from_dev
        self.timeout = 0.01

    @property
    def in_waiting(self) -> int:
        return len(self._from.buf)

    def read(self, n: int = 1) -> bytes:
        return self._from.get(n, self.timeout)

    def write(self, data: bytes) -> int:
        self._to.put(bytes(data))
        return len(data)

    def flush(self):
        pass

    def reset_input_buffer(self):
        self._from.clear()

    def close(self):
        pass


class FakeDevice:
    def __init__(self, pty: bool = False):
        self.path = FAKE_PORT
        self._to_dev, self._from_dev = _Pipe(), _Pipe()
        self.master = None
        if pty:
            self.master, slave = os.openpty()
            self.path = os.ttyname(slave)
        self.store: dict[int, bytes] = {}
        self.settings: dict[str, str] = {"len": "2.00", "dens": "60", "bri": "255", "gamma": "2.40", "mode": "full",
                                         "dir": "img", "col": "0", "gap": "img", "auto": "on", "speed": "img", "amps": "0"}
        self.current = 1
        self.test = False
        self.commands: list[str] = []
        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._write("\n-- POV strip 2.1 (fake) --\nready\n")

    def endpoint(self) -> _Endpoint:
        """What the host opens: Device("fake://", transport=fake.endpoint())."""
        return _Endpoint(self._to_dev, self._from_dev)

    def close(self):
        self._stop = True
        self._to_dev.close()
        self._from_dev.close()
        if self.master is not None:
            os.close(self.master)

    def _write(self, s: str | bytes):
        data = s.encode() if isinstance(s, str) else s
        if self.master is not None:
            os.write(self.master, data)
        else:
            self._from_dev.put(data)

    def _read_raw(self, n: int) -> bytes:
        if self.master is not None:
            return os.read(self.master, n)
        while not self._stop:
            b = self._to_dev.get(n, 0.2)
            if b:
                return b
        return b""

    def _read_exact(self, n: int) -> bytes:
        buf = b""
        deadline = time.monotonic() + 3.0
        while len(buf) < n and time.monotonic() < deadline:
            chunk = self._read_raw(n - len(buf))
            if not chunk:
                if self._stop:
                    break
                continue
            buf += chunk
        return buf

    def _run(self):
        line = b""
        while not self._stop:
            try:
                b = self._read_raw(1)
            except OSError:
                return
            if not b:
                continue
            if b in (b"\n", b"\r"):
                if line:
                    self._handle(line.decode("utf-8", "replace"))
                    line = b""
            else:
                line += b

    def _info(self):
        n = len(self.store)
        self._write(f"\n-- POV strip 2.1 (fake) --\nchip WS2812B (single-wire)\nstrip {self.settings['len']} m x "
                    f"{self.settings['dens']}/m = 120 LEDs (max 720)\nimage {self.current}/{max(1, n)}"
                    f"{'' if n else ' (compiled-in)'}: {'slot' if n else 'Heart'}, 120 rows x 16 cols\n"
                    f"column 3905 us (256 Hz); 16 columns per pass = 62.5 ms; gap "
                    f"{'20.9 ms (image)' if self.settings['gap'] == 'img' else f'{int(self.settings["gap"]) / 1000:.1f} ms'}\n"
                    f"sweep {self.settings['speed']}{' deg/s (image designed for 300: pass and gap x1.00)' if self.settings['speed'] != 'img' else ': the image\'s design speed, 300 deg/s'}\n"
                    f"current limit {'off; brightest column 0.31 A (20 mA per channel at full)' if float(self.settings['amps']) == 0 else f'{float(self.settings["amps"]):.2f} A: brightest column 0.31 A at bri {self.settings["bri"]}, not limiting'}\n"
                    f"dir {'ltr (image)' if self.settings['dir'] == 'img' else self.settings['dir']}, render {self.settings['mode']}, gamma {self.settings['gamma']}, bri {self.settings['bri']}, auto {self.settings['auto']}\n")

    def _handle(self, text: str):
        self.commands.append(text)
        parts = text.split()
        cmd, args = parts[0].lower(), parts[1:]
        if cmd == "ping":
            self._write("ok\n")
        elif cmd == "ver":
            self._write("pov 2.1 fake\nok\n")
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
        elif cmd in ("len", "dens", "bri", "gamma", "mode", "dir", "col", "gap", "auto", "img", "mhz", "speed", "amps"):
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
    dev = FakeDevice(pty=True)
    print(dev.path, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        pass
