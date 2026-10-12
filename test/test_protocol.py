"""Protocol round trip against the simulated strip.  python3 -m pytest test/  or  python3 test/test_protocol.py"""
import os
import sys
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from povstrip.fakedev import FakeDevice  # noqa: E402
from povstrip.protocol import Device, DeviceError, PovHeader, pov_file  # noqa: E402


def test_round_trip():
    fake = FakeDevice()
    time.sleep(0.2)
    dev = Device(fake.path)
    dev.connect()
    assert dev.info()[0].startswith("-- POV strip")
    rgb = bytes((i * 7) & 255 for i in range(120 * 16 * 3))
    data = pov_file(PovHeader(rows=120, cols=16, col_us=3905, gap_us=20853, dir=0, hold_s=8, length_m=2.0, name="Heart"), rgb)
    dev.put(1, data)
    assert fake.store[1] == data
    assert PovHeader.unpack(fake.store[1]).name == "Heart"
    assert any("Heart" in l for l in dev.ls())
    dev.rm(1)
    assert 1 not in fake.store
    lines, ok, err = dev.cmd("rm 9")
    assert not ok and "slot" in err
    # a lie about the crc must be refused
    dev._buf = b""
    dev._write(f"put 1 {len(data)} deadbeef\n".encode())
    assert dev._readline(2).strip() == "go"
    for i in range(0, len(data), 1024):
        dev._write(data[i:i + 1024])
        assert dev._readline(2).strip() == "."
    assert dev._readline(2).strip() == "err crc"
    assert 1 not in fake.store
    dev.apply(["len 2.5", "bri 200", "save"])
    assert fake.settings["len"] == "2.5"
    dev.close()
    fake.close()


if __name__ == "__main__":
    test_round_trip()
    print("ok")
