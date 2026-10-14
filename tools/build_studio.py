#!/usr/bin/env python3
"""Builds studio/pov-strip-studio.html from studio/studio.src.html + studio/core.js.
The firmware sources are inlined as JSON so the page can export a project zip.
Run from anywhere: python3 tools/build_studio.py"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "studio", "pov-strip-studio.html")

src = open(os.path.join(ROOT, "studio", "studio.src.html"), encoding="utf-8").read()
core = open(os.path.join(ROOT, "studio", "core.js"), encoding="utf-8").read()
files = {}
for rel in ["platformio.ini", "README.md", "pov_strip/pov_strip.ino", "pov_strip/config.h",
            "pov_strip/led_out.h", "pov_strip/pov_types.h", "pov_strip/pov_store.h", "pov_strip/pov_resample.h"]:
    files[rel] = open(os.path.join(ROOT, rel), encoding="utf-8").read()
fw = json.dumps({"files": files}, ensure_ascii=False).replace("<", "\\u003c")
assert "</script" not in core.lower()
out = src.replace("/*__FW_JSON__*/", fw).replace("/*__CORE_JS__*/", core)
assert "/*__" not in out
open(OUT, "w", encoding="utf-8").write(out)
print(OUT, len(out.encode("utf-8")), "bytes")
