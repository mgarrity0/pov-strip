"""Local web app: serves the Studio page and bridges it to the strip over serial.

    povstrip serve            http://127.0.0.1:8765, opens the browser
    povstrip serve --fake     a simulated strip, for trying the app without hardware

API (JSON): GET /api/ping, GET /api/ports, GET /api/status, POST /api/connect {port},
POST /api/disconnect, POST /api/cmd {text}, POST /api/put {slot, header, rgb_b64},
POST /api/wipe, GET /api/ls, GET /api/info, POST /api/flash {port, board},
GET /api/flash/status, GET /api/log, POST /api/quit

The Studio page is re-read from disk whenever it changes, so a `git pull` shows up on
the next reload without restarting. A second `povstrip serve` asks the running one to
quit and takes its port; if that fails it moves to the next free port.
"""
from __future__ import annotations

import base64
import json
import os
import socket
import threading
import time
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Optional

from . import __version__
from .protocol import Device, DeviceError, PovHeader, pov_file, usb_ports

STUDIO = Path(__file__).resolve().parent.parent / "studio" / "pov-strip-studio.html"


class Bridge:
    def __init__(self, fake: bool = False):
        self.lock = threading.Lock()
        self.dev: Optional[Device] = None
        self.port = ""
        self.fake = None
        self.log: list[str] = []
        self.flash_state: dict = {"running": False, "ok": None, "lines": [], "port": "", "board": ""}
        if fake:
            from .fakedev import FakeDevice
            self.fake = FakeDevice()

    def _log(self, s: str):
        self.log.append(s)
        del self.log[:-200]

    def ports(self):
        out = usb_ports()
        if self.fake:
            out.insert(0, {"device": self.fake.path, "description": "simulated strip", "score": 9})
        return out

    def connect(self, port: str, baud: int = 115200) -> list[str]:
        with self.lock:
            self.disconnect_locked()
            transport = self.fake.endpoint() if (self.fake and port == self.fake.path) else None
            dev = Device(port, baud, log=self._log, transport=transport)
            try:
                banner = dev.connect()
            except Exception:
                dev.close()
                raise
            self.dev, self.port = dev, port
            return banner

    def disconnect_locked(self):
        if self.dev:
            self.dev.close()
        self.dev, self.port = None, ""

    def disconnect(self):
        with self.lock:
            self.disconnect_locked()

    def require(self) -> Device:
        if not self.dev:
            raise DeviceError("not connected")
        return self.dev

    # ---- firmware flashing (arduino-cli) in a background thread ----
    def start_flash(self, port: str, board: str) -> None:
        if self.fake:
            raise DeviceError("simulated strip: nothing to flash")
        st = self.flash_state
        if st["running"]:
            raise DeviceError("a flash is already running")
        if not port:
            raise DeviceError("pick the serial port first")
        self.disconnect()  # the upload needs the port
        st.update(running=True, ok=None, lines=[], port=port, board=board)

        def work():
            from .flash import flash
            try:
                flash(board, port, log=lambda line: st["lines"].append(line))
                st["ok"] = True
            except BaseException as e:  # SystemExit from flash() too
                st["lines"].append(f"error: {e}")
                st["ok"] = False
            finally:
                st["running"] = False

        threading.Thread(target=work, daemon=True).start()


class _Page:
    """The Studio page, re-read when its file changes."""

    def __init__(self):
        self.mtime = None
        self.data = b""

    def get(self) -> bytes:
        try:
            m = STUDIO.stat().st_mtime_ns
        except OSError:
            return b"<p>studio/pov-strip-studio.html is missing; run python3 tools/build_studio.py</p>"
        if m != self.mtime:
            self.data, self.mtime = STUDIO.read_bytes(), m
        return self.data


def make_handler(bridge: Bridge, quit_event: threading.Event):
    page = _Page()

    class H(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):  # quiet
            pass

        def _json(self, obj, status=200):
            body = json.dumps(obj).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n) or b"{}")

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                body = page.get()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return
            try:
                if self.path == "/api/ping":
                    return self._json({"ok": True, "version": __version__, "fake": bool(bridge.fake)})
                if self.path == "/api/ports":
                    return self._json({"ok": True, "ports": bridge.ports()})
                if self.path == "/api/status":
                    return self._json({"ok": True, "connected": bridge.dev is not None, "port": bridge.port})
                if self.path == "/api/ls":
                    with bridge.lock:
                        return self._json({"ok": True, "lines": bridge.require().ls()})
                if self.path == "/api/info":
                    with bridge.lock:
                        return self._json({"ok": True, "lines": bridge.require().info()})
                if self.path == "/api/log":
                    return self._json({"ok": True, "lines": bridge.log[-60:]})
                if self.path == "/api/flash/status":
                    st = bridge.flash_state
                    return self._json({"ok": True, "running": st["running"], "result": st["ok"], "lines": st["lines"][-80:],
                                       "count": len(st["lines"]), "port": st["port"], "board": st["board"]})
            except DeviceError as e:
                return self._json({"ok": False, "error": str(e)})
            except Exception as e:  # serial errors etc.
                return self._json({"ok": False, "error": f"{type(e).__name__}: {e}"})
            self.send_error(404)

        def do_POST(self):
            try:
                body = self._body()
                if self.path == "/api/connect":
                    banner = bridge.connect(body.get("port", ""), int(body.get("baud", 115200)))
                    return self._json({"ok": True, "port": bridge.port, "banner": banner})
                if self.path == "/api/disconnect":
                    bridge.disconnect()
                    return self._json({"ok": True})
                if self.path == "/api/quit":
                    self._json({"ok": True})
                    quit_event.set()
                    return
                if self.path == "/api/flash":
                    bridge.start_flash(str(body.get("port", "")), str(body.get("board", "esp32dev")))
                    return self._json({"ok": True})
                if self.path == "/api/cmd":
                    with bridge.lock:
                        lines, ok, err = bridge.require().cmd(str(body.get("text", "")))
                    return self._json({"ok": ok, "lines": lines, "error": err})
                if self.path == "/api/wipe":
                    with bridge.lock:
                        bridge.require().wipe()
                    return self._json({"ok": True})
                if self.path == "/api/put":
                    h = body["header"]
                    hdr = PovHeader(int(h["rows"]), int(h["cols"]), int(h["col_us"]), int(h["gap_us"]), int(h.get("dir", 0)),
                                    int(h.get("hold_s", 8)), float(h.get("length_m", 0)), str(h.get("name", "")))
                    data = pov_file(hdr, base64.b64decode(body["rgb_b64"]))
                    with bridge.lock:
                        bridge.require().put(int(body["slot"]), data)
                    return self._json({"ok": True, "bytes": len(data)})
            except DeviceError as e:
                return self._json({"ok": False, "error": str(e)})
            except Exception as e:
                return self._json({"ok": False, "error": f"{type(e).__name__}: {e}"})
            self.send_error(404)

    return H


def _port_free(host: str, port: int) -> bool:
    """Nothing accepts connections on the port. (A bind probe would pass on Windows even
    while another process listens, because HTTPServer sets SO_REUSEADDR.)"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex((host, port)) != 0


def _retire_old_server(host: str, port: int) -> bool:
    """If a povstrip server answers on the port, ask it to quit. True when the port is free."""
    base = f"http://{host}:{port}"
    try:
        with urllib.request.urlopen(base + "/api/ping", timeout=1.5) as r:
            info = json.loads(r.read() or b"{}")
    except Exception:
        return False
    if not (isinstance(info, dict) and info.get("ok") and "version" in info):
        return False
    print(f"povstrip {info.get('version')} is already serving on port {port}: asking it to quit")
    try:
        req = urllib.request.Request(base + "/api/quit", data=b"{}", method="POST",
                                     headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=1.5).read()
    except Exception:
        print("  it predates /api/quit; close its window to free the port")
        return False
    for _ in range(40):
        if _port_free(host, port):
            return True
        time.sleep(0.1)
    return False


def serve(host: str = "127.0.0.1", port: int = 8765, fake: bool = False, open_browser: bool = True,
          serial_port: Optional[str] = None) -> None:
    if not _port_free(host, port) and not _retire_old_server(host, port):
        p = port
        while not _port_free(host, p):
            p += 1
        print(f"port {port} is busy: using {p}")
        port = p
    bridge = Bridge(fake=fake)
    if serial_port or fake:
        try:
            bridge.connect(serial_port or bridge.fake.path)
            print(f"connected to {bridge.port}")
        except Exception as e:
            print(f"not connected yet ({e}); use the Connect button in the page")
    quit_event = threading.Event()
    httpd = ThreadingHTTPServer((host, port), make_handler(bridge, quit_event))
    url = f"http://{host}:{port}/"
    print(f"POV Strip Studio at {url}  (Ctrl-C to stop)")
    if open_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    threading.Thread(target=lambda: (quit_event.wait(), httpd.shutdown()), daemon=True).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.disconnect()
        if bridge.fake:
            bridge.fake.close()
        httpd.server_close()
        if quit_event.is_set():
            print("stopped: another povstrip serve took over")
