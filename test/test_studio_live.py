"""Studio live-tuning panel against the simulated strip, in headless Chromium.

    python3 test/test_studio_live.py        (needs: pip install playwright && playwright install chromium)
"""
import os, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("skipped: playwright not installed")
    sys.exit(0)
from http.server import ThreadingHTTPServer
from povstrip.server import Bridge, make_handler
from povstrip.protocol import PovHeader

bridge = Bridge(fake=True)
httpd = ThreadingHTTPServer(("127.0.0.1", 8793), make_handler(bridge, threading.Event()))
threading.Thread(target=httpd.serve_forever, daemon=True).start()
fake = bridge.fake

fails = []
def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond: fails.append(msg)

with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page()
    errors = []
    pg.on("pageerror", lambda e: errors.append(str(e)))
    pg.goto("http://127.0.0.1:8793/")
    pg.wait_for_selector("#devicePanel:not([hidden])", timeout=5000)
    check(pg.is_disabled("#tuneSpeed"), "tune controls disabled before connect")
    pg.select_option("#devPort", "fake://")
    pg.click("#devConnect")
    pg.wait_for_function("document.querySelector('#devLive').classList.contains('on')", timeout=10000)
    time.sleep(0.5)
    check(not pg.is_disabled("#tuneSpeed"), "tune controls enabled after connect")
    check(pg.input_value("#tuneSpeed") == "300", "speed slider shows the strip's design speed (300): " + pg.input_value("#tuneSpeed"))
    check(pg.input_value("#tuneGap") == "0", "gap slider at 'as uploaded'")
    check(pg.input_value("#tuneDir") == "img", "dir select 'as uploaded'")
    n0 = len(fake.commands)
    # drag the speed slider: set value + input event
    pg.eval_on_selector("#tuneSpeed", "el => { el.value = 150; el.dispatchEvent(new Event('input', {bubbles: true})); }")
    pg.eval_on_selector("#tuneSpeed", "el => { el.value = 120; el.dispatchEvent(new Event('input', {bubbles: true})); }")
    time.sleep(0.8)
    sent = fake.commands[n0:]
    check(sent == ["speed 120"], f"speed slider coalesces to one command: {sent}")
    check("live, not saved" in pg.text_content("#tuneState"), "state line shows the effect: " + pg.text_content("#tuneState"))
    n0 = len(fake.commands)
    pg.eval_on_selector("#tuneGap", "el => { el.value = 60; el.dispatchEvent(new Event('input', {bubbles: true})); }")
    pg.select_option("#tuneDir", "pal")
    pg.eval_on_selector("#tuneBri", "el => { el.value = 128; el.dispatchEvent(new Event('input', {bubbles: true})); }")
    time.sleep(1.0)
    sent = fake.commands[n0:]
    check(sorted(sent) == sorted(["gap 60000", "dir pal", "bri 128"]), f"gap/dir/bri sent: {sent}")
    check(pg.input_value("#bri") == "128", "design brightness follows the live slider")
    # Apply strip settings carries the live values and saves
    n0 = len(fake.commands)
    pg.click("#devApply")
    pg.wait_for_function("!document.querySelector('#devApply').disabled", timeout=10000)
    sent = fake.commands[n0:]
    check("speed 120" in sent and "gap 60000" in sent and "dir pal" in sent and "bri 128" in sent and sent[-2:] == ["save", "info"] or sent[-1] in ("save", "info"), f"apply sends live values then save: {sent}")
    # Send ticked slides: header carries the design speed
    n0 = len(fake.commands)
    pg.click("#devSend")
    pg.wait_for_function("!document.querySelector('#devSend').disabled", timeout=20000)
    check(1 in fake.store, "slot 1 uploaded")
    if 1 in fake.store:
        h = PovHeader.unpack(fake.store[1])
        check(h.sweep_dps == 300 and fake.store[1][17] == 60, f"uploaded header sweep {h.sweep_dps} deg/s (byte 17 = {fake.store[1][17]})")
    # reconnect: the page adopts the strip's live values
    pg.click("#devConnect")  # disconnect
    time.sleep(0.5)
    fake.settings["speed"] = "200"; fake.settings["dir"] = "rtl"
    pg.click("#devConnect")
    pg.wait_for_function("document.querySelector('#devLive').classList.contains('on')", timeout=10000)
    time.sleep(0.5)
    check(pg.input_value("#tuneSpeed") == "200", "reconnect adopts the strip's sweep speed: " + pg.input_value("#tuneSpeed"))
    check(pg.input_value("#tuneDir") == "rtl", "reconnect adopts the strip's direction: " + pg.input_value("#tuneDir"))
    check(not errors, f"no page errors: {errors[:3]}")
    b.close()
httpd.shutdown()
print("FAILED" if fails else "ALL OK")
