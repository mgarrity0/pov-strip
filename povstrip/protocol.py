"""Serial protocol client for the pov-strip firmware (see pov_strip/pov_store.h).

Every console command is a text line; the device answers with text and a final
"ok" / "ok <n>" / "err <reason>" line. Uploads ("put") stream a .pov file in
1024-byte chunks, each acknowledged with a "." line.
"""
from __future__ import annotations

import struct
import time
import zlib
from dataclasses import dataclass
from typing import Callable, Iterable, Optional

import serial
from serial.tools import list_ports

HEADER_BYTES = 48
CHUNK = 1024
SLOTS = 32
DIRS = {"ltr": 0, "rtl": 1, "alt": 2, "pal": 3}


class DeviceError(Exception):
    pass


@dataclass
class PovHeader:
    rows: int
    cols: int
    col_us: int
    gap_us: int
    dir: int = 0
    hold_s: int = 8
    length_m: float = 0.0
    name: str = ""

    def pack(self) -> bytes:
        name = self.name.encode("utf-8")[:23].ljust(24, b"\0")
        return struct.pack("<4sHHIIBBHf24s", b"POV1", self.rows, self.cols, self.col_us, self.gap_us,
                           self.dir, 0, self.hold_s, float(self.length_m), name)

    @staticmethod
    def unpack(b: bytes) -> "PovHeader":
        magic, rows, cols, col_us, gap_us, d, _r, hold, length, name = struct.unpack("<4sHHIIBBHf24s", b[:HEADER_BYTES])
        if magic != b"POV1":
            raise DeviceError("not a POV1 file")
        return PovHeader(rows, cols, col_us, gap_us, d, hold, length, name.rstrip(b"\0").decode("utf-8", "replace"))


def pov_file(header: PovHeader, rgb: bytes) -> bytes:
    """A complete .pov file: header + column-major RGB888 (rows*cols*3 bytes)."""
    if len(rgb) != header.rows * header.cols * 3:
        raise DeviceError(f"pixel data is {len(rgb)} bytes, expected {header.rows * header.cols * 3}")
    return header.pack() + bytes(rgb)


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def usb_ports() -> list[dict]:
    """Serial ports that look like USB adapters, most likely ESP32 boards first."""
    out = []
    for p in list_ports.comports():
        desc = (p.description or "") + " " + (p.manufacturer or "")
        score = 0
        for key in ("CP210", "CH340", "CH9102", "FTDI", "USB", "Silicon Labs", "wch", "Espressif", "JTAG"):
            if key.lower() in desc.lower():
                score += 1
        if p.vid is not None:
            score += 1
        out.append({"device": p.device, "description": p.description or "", "score": score})
    out.sort(key=lambda x: (-x["score"], x["device"]))
    return out


def pick_port(explicit: Optional[str] = None) -> str:
    if explicit:
        return explicit
    ports = [p for p in usb_ports() if p["score"] > 0]
    if len(ports) == 1:
        return ports[0]["device"]
    if not ports:
        raise DeviceError("no USB serial port found; plug the ESP32 in or pass --serial PORT")
    names = ", ".join(p["device"] for p in ports)
    raise DeviceError(f"several serial ports ({names}); pass --serial PORT")


class Device:
    """One open serial connection to the strip."""

    def __init__(self, port: str, baud: int = 115200, log: Optional[Callable[[str], None]] = None, transport=None):
        self.port = port
        self.baud = baud
        self.log = log or (lambda s: None)
        self.banner: list[str] = []
        if transport is not None:  # a simulated strip (fakedev.FakeDevice.endpoint())
            self.ser = transport
        else:
            self.ser = serial.Serial()
            self.ser.port = port
            self.ser.baudrate = baud
            self.ser.timeout = 0.01
            self.ser.write_timeout = 5
            # Try not to reset the board (DTR/RTS drive the ESP32 auto-reset circuit).
            # Many drivers pulse them anyway; connect() copes with a reboot.
            self.ser.dtr = False
            self.ser.rts = False
            self.ser.open()
        self._buf = b""

    # ---- low level -----------------------------------------------------------------
    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, timeout: float) -> Optional[str]:
        end = time.monotonic() + timeout
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                line, self._buf = self._buf[:i], self._buf[i + 1:]
                return line.decode("utf-8", "replace").rstrip("\r")
            if time.monotonic() > end:
                return None
            chunk = self.ser.read(max(1, self.ser.in_waiting))
            if chunk:
                self._buf += chunk

    def _drain(self, quiet: float = 0.15) -> list[str]:
        lines = []
        while True:
            line = self._readline(quiet)
            if line is None:
                return lines
            lines.append(line)

    def _write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    # ---- session ----------------------------------------------------------------------
    def connect(self, timeout: float = 8.0) -> list[str]:
        """Waits for the firmware, whether it just rebooted or was already running."""
        end = time.monotonic() + timeout
        self.banner = []
        # a board that just reset prints its banner and ends with "ready"
        while time.monotonic() < end:
            line = self._readline(0.4)
            if line is None:
                break
            self.banner.append(line)
            if line.strip() == "ready":
                break
        # either way, a ping must answer
        for _ in range(3):
            lines, ok, err = self.cmd("ping", timeout=2.0)
            if ok:
                return self.banner
            self.banner += lines
        raise DeviceError("no answer from the strip (is pov-strip firmware 2.0 flashed? right port?)")

    def cmd(self, text: str, timeout: float = 6.0) -> tuple[list[str], bool, str]:
        """Sends one console command. Returns (output lines, ok, error message)."""
        self.log(f"> {text}")
        self._buf = b""
        self.ser.reset_input_buffer()
        self._write(text.strip().encode("utf-8") + b"\n")
        lines: list[str] = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self._readline(end - time.monotonic())
            if line is None:
                break
            s = line.strip()
            if s == "ok" or s.startswith("ok "):
                return lines, True, ""
            if s.startswith("err"):
                return lines, False, s[3:].strip() or "error"
            if s:
                lines.append(line)
                self.log(f"  {line}")
        return lines, False, "timeout"

    def info(self) -> list[str]:
        lines, ok, err = self.cmd("info")
        if not ok:
            raise DeviceError(err)
        return lines

    def ls(self) -> list[str]:
        lines, ok, err = self.cmd("ls")
        if not ok:
            raise DeviceError(err)
        return lines

    def wipe(self) -> None:
        _, ok, err = self.cmd("wipe", timeout=20)
        if not ok:
            raise DeviceError(err)

    def rm(self, slot: int) -> None:
        _, ok, err = self.cmd(f"rm {slot}")
        if not ok:
            raise DeviceError(err)

    def apply(self, commands: Iterable[str]) -> list[str]:
        out = []
        for c in commands:
            lines, ok, err = self.cmd(c)
            out += lines
            if not ok:
                raise DeviceError(f"{c}: {err}")
        return out

    def put(self, slot: int, data: bytes, progress: Optional[Callable[[int, int], None]] = None) -> None:
        """Uploads one .pov file into slot 1..32."""
        if not 1 <= slot <= SLOTS:
            raise DeviceError("slot must be 1..32")
        PovHeader.unpack(data)  # validates
        total = len(data)
        self.log(f"> put {slot} {total} {crc32(data):08x}")
        self._buf = b""
        self.ser.reset_input_buffer()
        self._write(f"put {slot} {total} {crc32(data):08x}\n".encode())
        # wait for go
        end = time.monotonic() + 6
        while True:
            line = self._readline(max(0.0, end - time.monotonic()))
            if line is None:
                raise DeviceError("no 'go' from the strip")
            s = line.strip()
            if s == "go":
                break
            if s.startswith("err"):
                raise DeviceError(s[3:].strip())
        sent = 0
        while sent < total:
            chunk = data[sent:sent + CHUNK]
            self._write(chunk)
            sent += len(chunk)
            ack_end = time.monotonic() + 8
            while True:
                line = self._readline(max(0.0, ack_end - time.monotonic()))
                if line is None:
                    raise DeviceError(f"no ack after {sent} of {total} bytes")
                s = line.strip()
                if s == ".":
                    break
                if s.startswith("err"):
                    raise DeviceError(s[3:].strip())
            if progress:
                progress(sent, total)
        end = time.monotonic() + 10
        while True:
            line = self._readline(max(0.0, end - time.monotonic()))
            if line is None:
                raise DeviceError("no result after upload")
            s = line.strip()
            if s.startswith("ok"):
                return
            if s.startswith("err"):
                raise DeviceError(s[3:].strip())
