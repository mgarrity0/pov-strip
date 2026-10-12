#!/usr/bin/env bash
# povstrip launcher for macOS/Linux: makes a private Python environment on first run,
# installs the tool into it, then runs the command you gave.
#   ./povstrip.sh ports | flash --serial /dev/cu.usbserial-XXXX | serve | push photo.png ...
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if [ ! -x "$HERE/.venv/bin/python" ]; then
  echo "Creating .venv and installing povstrip (first run only)..."
  python3 -m venv "$HERE/.venv"
  "$HERE/.venv/bin/python" -m pip install --quiet --upgrade pip
  "$HERE/.venv/bin/python" -m pip install --quiet -e "$HERE"
fi
exec "$HERE/.venv/bin/python" -m povstrip "$@"
