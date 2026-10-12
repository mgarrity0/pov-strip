"""Python resampler and design math vs studio/core.js (needs node).  python3 test/test_core_vs_js.py"""
import json
import os
import subprocess
import sys

import numpy as np

ROOT = os.path.join(os.path.dirname(__file__), "..")
sys.path.insert(0, ROOT)
from povstrip import core  # noqa: E402

JS = """
const P=require(process.argv[1]); let d=""; process.stdin.on("data",c=>d+=c).on("end",()=>{const a=JSON.parse(d);
const out=P.areaResample(Uint8Array.from(a.src),3,a.sw,a.sh,a.dw,a.dh,a.top,a.dh2,a.g);
const des=P.design({lengthM:2,density:60,distM:5,sweepDps:300,glanceDeg:25,colMode:"auto",colUsManual:0,chip:"WS2812B",spiMHz:16});
process.stdout.write(JSON.stringify({out:Array.from(out), n:des.n, chipUs:des.chipUs, colUs:des.colUs, cols:P.columnsFor(des,1,0.8)}));});
"""


def main():
    d = core.design(2, 60, 5, 300, 25, "WS2812B")
    rng = np.random.default_rng(1)
    worst = 0
    for (sh, sw, dw, dh, top, dh2, g) in [(96, 120, 16, 120, 0, 120, 2.4), (210, 168, 96, 120, 10, 100, 2.2), (7, 13, 3, 5, 0, 5, 1.0)]:
        src = rng.integers(0, 256, (sh, sw, 3), dtype=np.uint8)
        py = core.area_resample(src, dw, dh, top, dh2, g)
        r = subprocess.run(["node", "-e", JS, os.path.join(ROOT, "studio", "core.js")], text=True, capture_output=True, check=True,
                           input=json.dumps({"src": src.flatten().tolist(), "sw": sw, "sh": sh, "dw": dw, "dh": dh, "top": top, "dh2": dh2, "g": g}))
        j = json.loads(r.stdout)
        js = np.array(j["out"], dtype=np.int16).reshape(dw, dh, 3)
        worst = max(worst, int(np.abs(js - py.astype(np.int16)).max()))
        assert (j["n"], j["chipUs"], j["colUs"], j["cols"]) == (d.n, d.chip_us, d.col_us, core.columns_for(d, 1, 0.8)), "design math differs"
    print(f"resampler max |py - js| = {worst}; design math agrees")
    assert worst <= 1


if __name__ == "__main__":
    main()
