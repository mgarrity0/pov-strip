"""Build and upload the firmware with arduino-cli (the engine inside Arduino IDE 2).

arduino-cli is found on PATH or at the usual Arduino IDE locations. Missing pieces
(the esp32 core, FastLED) are installed on demand.
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


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    print("$", " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=check)


def ensure_core_and_libs(cli: list[str], single_wire: bool) -> None:
    cores = subprocess.run(cli + ["core", "list", "--format", "json"], capture_output=True, text=True)
    if "esp32:esp32" not in cores.stdout:
        run(cli + ["core", "update-index", "--additional-urls", ESP32_INDEX])
        run(cli + ["core", "install", "esp32:esp32", "--additional-urls", ESP32_INDEX])
    if single_wire:
        libs = subprocess.run(cli + ["lib", "list", "FastLED", "--format", "json"], capture_output=True, text=True)
        if "FastLED" not in libs.stdout:
            run(cli + ["lib", "install", "FastLED"])


def chip_is_single_wire() -> bool:
    cfg = (SKETCH / "config.h").read_text(encoding="utf-8")
    for line in cfg.splitlines():
        if line.startswith("#define POV_CHIP"):
            return not any(k in line for k in ("APA102", "SK9822", "HD107S"))
    return True


def flash(board: str, serial_port: str | None, build_only: bool = False, huge: bool | None = None) -> None:
    cli = find_cli()
    if board not in BOARDS:
        raise SystemExit(f"board must be one of {', '.join(BOARDS)}")
    fqbn, opts = BOARDS[board]
    if huge is False:
        opts = ""
    ensure_core_and_libs(cli, chip_is_single_wire())
    cmd = cli + ["compile", "--fqbn", fqbn]
    if opts:
        cmd += ["--board-options", opts]
    if not build_only:
        if not serial_port:
            raise SystemExit("pass --serial PORT to upload (povstrip ports lists them)")
        cmd += ["--upload", "-p", serial_port]
    cmd.append(str(SKETCH))
    run(cmd)
    print("done" if build_only else "flashed; run `povstrip serve` or `povstrip info`")
