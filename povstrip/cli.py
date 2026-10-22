"""povstrip command line.

    povstrip serve                 Studio in the browser, strip over USB
    povstrip flash --serial PORT   build + upload the firmware (arduino-cli)
    povstrip push photo.png        render for the strip and upload into a slot
    povstrip info | ls | wipe | rm N | cmd "len 2.5" | ports | monitor
"""
from __future__ import annotations

import argparse
import sys
import time

from . import __version__
from .protocol import Device, DeviceError, PovHeader, pov_file, pick_port, usb_ports


def _dev(args) -> Device:
    port = pick_port(args.serial)
    dev = Device(port, args.baud)
    dev.connect()
    return dev


def cmd_ports(args):
    for p in usb_ports():
        print(f"{p['device']:<28} {p['description']}")
    if not usb_ports():
        print("no serial ports found")


def cmd_serve(args):
    from .server import serve
    serve(port=args.http_port, fake=args.fake, open_browser=not args.no_browser, serial_port=args.serial)


def cmd_flash(args):
    from .flash import flash
    cfg = {k: v for k, v in (("chip", args.chip), ("pinData", args.data), ("pinClock", args.clock),
                             ("lengthM", args.len), ("density", args.dens)) if v is not None}
    flash(args.board, args.serial, build_only=args.build_only, huge=(False if args.default_partition else None),
          config=({} if args.defaults else cfg or None))


def cmd_info(args):
    dev = _dev(args)
    try:
        print("\n".join(dev.info()))
    finally:
        dev.close()


def cmd_ls(args):
    dev = _dev(args)
    try:
        print("\n".join(dev.ls()))
    finally:
        dev.close()


def cmd_wipe(args):
    dev = _dev(args)
    try:
        dev.wipe()
        print("store emptied")
    finally:
        dev.close()


def cmd_rm(args):
    dev = _dev(args)
    try:
        dev.rm(args.slot)
        print(f"slot {args.slot} removed")
    finally:
        dev.close()


def cmd_cmd(args):
    dev = _dev(args)
    try:
        lines, ok, err = dev.cmd(" ".join(args.text))
        print("\n".join(lines))
        if not ok:
            print(f"err {err}", file=sys.stderr)
            sys.exit(1)
    finally:
        dev.close()


def cmd_send(args):
    data = open(args.file, "rb").read()
    h = PovHeader.unpack(data)
    dev = _dev(args)
    try:
        dev.put(args.slot, data, progress=lambda a, b: print(f"\r{a}/{b} bytes", end="", flush=True))
        print(f"\nslot {args.slot}: {h.name or args.file} {h.rows}x{h.cols}")
    finally:
        dev.close()


def cmd_push(args):
    from . import core
    d = core.design(args.len, args.dens, args.dist, args.eye, args.glance, args.chip, args.spi)
    rgb, cols = core.render_image_file(args.image, d, scale=args.height, gamma=args.gamma, invert=args.invert,
                                       pixel_art=args.pixel)
    pass_ms = cols * d.col_us / 1000
    gap_us = round(core.recommended_gap_ms(d.glance_ms, pass_ms) * 1000) if args.gap is None else int(args.gap * 1000)
    name = args.name or args.image.rsplit("/", 1)[-1].rsplit(".", 1)[0]
    hdr = PovHeader(rows=d.n, cols=cols, col_us=d.col_us, gap_us=gap_us, dir={"ltr": 0, "rtl": 1, "alt": 2, "pal": 3}[args.dir],
                    hold_s=args.hold, length_m=args.len, name=name, sweep_dps=int(round(args.eye)))
    data = pov_file(hdr, rgb.tobytes())
    print(f"{name}: {d.n} LEDs x {cols} columns, {d.col_us} us per column"
          f"{' (chip-limited)' if d.chip_limited else ''}, pass {pass_ms:.1f} ms, gap {gap_us / 1000:.1f} ms, {len(data)} bytes")
    if args.out:
        open(args.out, "wb").write(data)
        print(f"wrote {args.out}")
        return
    dev = _dev(args)
    try:
        t0 = time.time()
        dev.put(args.slot, data, progress=lambda a, b: print(f"\r{a}/{b} bytes", end="", flush=True))
        print(f"\nuploaded into slot {args.slot} in {time.time() - t0:.1f} s")
    finally:
        dev.close()


def cmd_monitor(args):
    port = pick_port(args.serial)
    import serial
    ser = serial.Serial(port, args.baud, timeout=0.1)
    print(f"monitor on {port} at {args.baud} (Ctrl-C to quit); type commands + Enter")
    import threading

    def reader():
        while True:
            b = ser.read(4096)
            if b:
                sys.stdout.write(b.decode("utf-8", "replace"))
                sys.stdout.flush()

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            ser.write(line.encode())
    except KeyboardInterrupt:
        pass
    ser.close()


def main(argv=None):
    ap = argparse.ArgumentParser(prog="povstrip", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"povstrip {__version__}")
    ap.add_argument("--serial", help="serial port (auto when only one USB port exists)")
    ap.add_argument("--baud", type=int, default=115200)
    sub = ap.add_subparsers(dest="command", required=True)

    sub.add_parser("ports", help="list serial ports").set_defaults(fn=cmd_ports)
    s = sub.add_parser("serve", help="Studio page in the browser, bridged to the strip")
    s.add_argument("--http-port", type=int, default=8765)
    s.add_argument("--fake", action="store_true", help="simulated strip (no hardware)")
    s.add_argument("--no-browser", action="store_true")
    s.set_defaults(fn=cmd_serve)
    f = sub.add_parser("flash", help="build and upload the firmware with arduino-cli")
    f.add_argument("--board", default="esp32dev", choices=["esp32dev", "sparklemotion", "xiao_esp32s3", "esp32s3_devkitc"])
    f.add_argument("--build-only", action="store_true")
    f.add_argument("--default-partition", action="store_true", help="keep the 1.25 MB app partition (Huge APP otherwise)")
    f.add_argument("--chip", help="LED chip, e.g. WS2812B or APA102 (writes pov_strip/config_local.h)")
    f.add_argument("--data", type=int, help="data GPIO number")
    f.add_argument("--clock", type=int, help="clock GPIO number (clocked chips)")
    f.add_argument("--len", type=float, help="default strip length m")
    f.add_argument("--dens", type=int, help="default LEDs per meter")
    f.add_argument("--defaults", action="store_true", help="remove config_local.h and build config.h's defaults")
    f.set_defaults(fn=cmd_flash)
    sub.add_parser("info", help="numbers for the current setup").set_defaults(fn=cmd_info)
    sub.add_parser("ls", help="images stored on the strip").set_defaults(fn=cmd_ls)
    sub.add_parser("wipe", help="remove every stored image").set_defaults(fn=cmd_wipe)
    r = sub.add_parser("rm", help="remove one slot")
    r.add_argument("slot", type=int)
    r.set_defaults(fn=cmd_rm)
    c = sub.add_parser("cmd", help="send one console command, e.g. cmd len 2.5")
    c.add_argument("text", nargs="+")
    c.set_defaults(fn=cmd_cmd)
    sd = sub.add_parser("send", help="upload a .pov file")
    sd.add_argument("file")
    sd.add_argument("--slot", type=int, default=1)
    sd.set_defaults(fn=cmd_send)
    p = sub.add_parser("push", help="render an image for the strip and upload it")
    p.add_argument("image")
    p.add_argument("--slot", type=int, default=1)
    p.add_argument("--name")
    p.add_argument("--height", type=float, default=1.0, help="fraction of the strip height (0.2 to 1)")
    p.add_argument("--hold", type=int, default=8, help="seconds before the playlist advances; 0 = stay")
    p.add_argument("--dir", default="ltr", choices=["ltr", "rtl", "alt", "pal"])
    p.add_argument("--gap", type=float, default=None, help="dark gap in ms (default: glance minus pass)")
    p.add_argument("--invert", action="store_true")
    p.add_argument("--pixel", action="store_true", help="pixel art: no smoothing when upscaling")
    p.add_argument("--len", type=float, default=2.0, help="strip length m")
    p.add_argument("--dens", type=int, default=60, help="LEDs per meter")
    p.add_argument("--dist", type=float, default=5.0, help="viewing distance m")
    p.add_argument("--eye", type=float, default=300.0, help="eye speed across the strip, deg/s")
    p.add_argument("--glance", type=float, default=25.0, help="glance size, deg")
    p.add_argument("--chip", default="WS2812B")
    p.add_argument("--spi", type=float, default=16.0, help="SPI MHz (clocked chips)")
    p.add_argument("--gamma", type=float, default=2.4)
    p.add_argument("--out", help="write the .pov file instead of uploading")
    p.set_defaults(fn=cmd_push)
    sub.add_parser("monitor", help="raw serial console").set_defaults(fn=cmd_monitor)

    args = ap.parse_args(argv)
    try:
        args.fn(args)
    except DeviceError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(2)
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
