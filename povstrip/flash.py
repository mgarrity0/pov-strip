"""Build and upload the firmware with arduino-cli (the engine inside Arduino IDE 2).

arduino-cli is found on PATH or at the usual Arduino IDE locations. The esp32 core is
installed on demand; the firmware needs no libraries.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ESP32_INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"
SKETCH = Path(__file__).resolve().parent.parent / "pov_strip"

BOARDS = {
    "esp32dev": ("esp32:esp32:esp32", "PartitionScheme=huge_app"),
    "sparklemotion": ("esp32:esp32:sparklemotion", "PartitionScheme=noota_3g"),   # 1 MB app, 3 MB for images
    "xiao_esp32s3": ("esp32:esp32:XIAO_ESP32S3", ""),
    "esp32s3_devkitc": ("esp32:esp32:esp32s3", "PartitionScheme=huge_app"),
}


def find_cli() -> list[str]:
    """arduino-cli on PATH, or the copy inside Arduino IDE 2 (with the IDE's config, so
    the cores and libraries installed from the IDE are the ones used)."""
    for name in ("arduino-cli", "arduino-cli.exe"):
        p = shutil.which(name)
        if p:
            return [p]
    ide_cfg = Path.home() / ".arduinoIDE" / "arduino-cli.yaml"
    local = Path(os.environ.get("LOCALAPPDATA", ""))
    candidates = [
        local / "Programs" / "Arduino IDE" / "resources" / "app" / "lib" / "backend" / "resources" / "arduino-cli.exe",
        Path("C:/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"),
        Path("/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"),
        local / "Programs" / "arduino-cli" / "arduino-cli.exe",
        Path("/usr/local/bin/arduino-cli"),
        Path("/opt/homebrew/bin/arduino-cli"),
    ]
    for c in candidates:
        if c.is_file():
            cmd = [str(c)]
            if "Arduino IDE" in str(c) and ide_cfg.is_file():
                cmd += ["--config-file", str(ide_cfg)]
            return cmd
    raise SystemExit(
        "arduino-cli not found. Install Arduino IDE 2 (its bundled arduino-cli is found automatically),\n"
        "or arduino-cli itself (macOS: brew install arduino-cli; Windows: winget install ArduinoSA.CLI;\n"
        "Linux: curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh)."
    )


def run(cmd: list[str], log=print) -> None:
    """Runs a command, streaming its output lines to `log`. Raises on failure."""
    log("$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    assert proc.stdout is not None
    for line in proc.stdout:
        log(line.rstrip("\n"))
    if proc.wait() != 0:
        raise RuntimeError(f"{os.path.basename(cmd[0])} {cmd[1] if len(cmd) > 1 else ''} failed (exit {proc.returncode})")


def ensure_core(cli: list[str], log=print) -> None:
    cores = subprocess.run(cli + ["core", "list", "--format", "json"], capture_output=True, text=True)
    if "esp32:esp32" not in cores.stdout:
        log("esp32 core not installed: installing (a few minutes, ~1 GB)")
        run(cli + ["core", "update-index", "--additional-urls", ESP32_INDEX], log)
        run(cli + ["core", "install", "esp32:esp32", "--additional-urls", ESP32_INDEX], log)


LOCAL = SKETCH / "config_local.h"
CHIPS = ("APA102", "SK9822", "HD107S", "WS2812B", "WS2812B_V5", "SK6812", "WS2811", "WS2813", "WS2815")
MODES = {"full": "RENDER_FULL", "safe": "RENDER_PWM_SAFE", "current": "RENDER_CURRENT"}


def local_config_text(cfg: dict) -> str:
    """config_local.h from the Studio's chip / pins / defaults. Unknown keys are ignored,
    bad values raise ValueError. Values not given keep config.h's defaults."""
    out = ["// config_local.h - written by povstrip from the Studio's Chip and Pins controls.",
           "// Overrides config.h; not tracked by git. Delete it to go back to the defaults.", "#pragma once"]

    def num(key, lo, hi, kind=int):
        if key not in cfg or cfg[key] in (None, ""):
            return None
        v = kind(cfg[key])
        if not (lo <= v <= hi):
            raise ValueError(f"{key} out of range: {v}")
        return v

    chip = cfg.get("chip")
    if chip:
        chip = str(chip).upper().replace("-", "_")
        if chip not in CHIPS:
            raise ValueError(f"unknown chip {chip}")
        out.append(f"#define POV_CHIP LED_{chip}")
    for key, macro, lo, hi, kind, fmt in (
        ("pinData", "POV_PIN_DATA", 0, 48, int, "{}"),
        ("pinClock", "POV_PIN_CLOCK", 0, 48, int, "{}"),
        ("lengthM", "POV_LENGTH_M", 0.05, 100, float, "{:.2f}f"),
        ("density", "POV_LEDS_PER_M", 1, 1000, int, "{}"),
        ("numLedsMax", "POV_NUM_LEDS_MAX", 1, 10000, int, "{}"),
        ("spiMHz", "POV_SPI_MHZ", 0.1, 40, float, "{:g}"),
        ("bri", "POV_BRIGHTNESS", 0, 255, int, "{}"),
        ("gamma", "POV_GAMMA", 0.5, 5, float, "{:.2f}f"),
    ):
        v = num(key, lo, hi, kind)
        if v is not None:
            out.append(f"#define {macro} " + fmt.format(v))
    if "dataInAtBottom" in cfg:
        out.append(f"#define POV_DATA_IN_AT_BOTTOM {1 if cfg['dataInAtBottom'] else 0}")
    order = cfg.get("order")
    if order:
        order = str(order).upper()
        if len(order) != 3 or set(order) != {"R", "G", "B"}:
            raise ValueError(f"bad color order {order}")
        macro = "POV_CLOCKED_ORDER" if (chip or "WS2812B") in ("APA102", "SK9822", "HD107S") else "POV_SINGLE_ORDER"
        out.append(f'#define {macro} "{order}"')
    mode = cfg.get("mode")
    if mode:
        if mode not in MODES:
            raise ValueError(f"bad render mode {mode}")
        out.append(f"#define POV_RENDER {MODES[mode]}")
    return "\n".join(out) + "\n"


def write_local_config(cfg: dict | None, log=print) -> None:
    """cfg None: leave config_local.h as it is. {}: remove it (config.h defaults)."""
    if cfg is None:
        return
    if not cfg:
        if LOCAL.exists():
            LOCAL.unlink()
            log("removed config_local.h: config.h defaults")
        return
    text = local_config_text(cfg)
    LOCAL.write_text(text, encoding="utf-8")
    log("config_local.h: " + "; ".join(l[8:] for l in text.splitlines() if l.startswith("#define")))


def flash(board: str, serial_port: str | None, build_only: bool = False, huge: bool | None = None, log=print,
          config: dict | None = None) -> None:
    cli = find_cli()
    write_local_config(config, log)
    if board not in BOARDS:
        raise SystemExit(f"board must be one of {', '.join(BOARDS)}")
    fqbn, opts = BOARDS[board]
    if huge is False:
        opts = ""
    log(f"arduino-cli: {cli[0]}")
    ensure_core(cli, log)
    cmd = cli + ["compile", "--fqbn", fqbn]
    if opts:
        cmd += ["--board-options", opts]
    if not build_only:
        if not serial_port:
            raise SystemExit("pass --serial PORT to upload (povstrip ports lists them)")
        cmd += ["--upload", "-p", serial_port]
    cmd.append(str(SKETCH))
    if LOCAL.exists() and config is None:
        log("using " + LOCAL.name + ": " + "; ".join(l[8:] for l in LOCAL.read_text(encoding="utf-8").splitlines() if l.startswith("#define")))
    run(cmd, log)
    log("done" if build_only else "flashed; the strip reboots with the new firmware")
