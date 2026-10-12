"""Local web app: serves the Studio page and bridges it to the strip over serial.

    povstrip serve            http://127.0.0.1:8765, opens the browser
    povstrip serve --fake     a simulated strip, for trying the app without hardware

API (JSON): GET /api/ping, GET /api/ports, GET /api/status, POST /api/connect {port},
POST /api/disconnect, POST /api/cmd {text}, POST /api/put {slot, header, rgb_b64},
POST /api/wipe, GET /api/ls, GET /api/info
"""
from __future__ import annotations

import base64
import json
import os
import threading
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
            dev = Device(port, baud, log=self._log)
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


def make_handler(bridge: Bridge):
    page = STUDIO.read_bytes() if STUDIO.exists() else b"<p>studio/pov-strip-studio.html is missing; run python3 tools/build_studio.py</p>"

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
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(page)
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


def serve(host: str = "127.0.0.1", port: int = 8765, fake: bool = False, open_browser: bool = True,
          serial_port: Optional[str] = None) -> None:
    bridge = Bridge(fake=fake)
    if serial_port or fake:
        try:
            bridge.connect(serial_port or bridge.fake.path)
            print(f"connected to {bridge.port}")
        except Exception as e:
            print(f"not connected yet ({e}); use the Connect button in the page")
    httpd = ThreadingHTTPServer((host, port), make_handler(bridge))
    url = f"http://{host}:{port}/"
    print(f"POV Strip Studio at {url}  (Ctrl-C to stop)")
    if open_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        bridge.disconnect()
        if bridge.fake:
            bridge.fake.close()
        httpd.server_close()
