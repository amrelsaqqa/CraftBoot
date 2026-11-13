#!/usr/bin/env python3
"""Find the EIP number RouterOS's crypto driver will accept.

    tools/ni-cryptosweep.py                     candidates, 4 guests at a time
    tools/ni-cryptosweep.py --all               sweep 0x00..0xff
    tools/ni-cryptosweep.py --values 0x93,0x97  explicit list

The driver reads the engine's number register (crypto@1e004000 + 0x1fc) and
panics with `Kernel panic - not syncing: EipNumber: <low byte>` if it does not
recognise it, so the accepted value can be found by booting the image with
different MT7621_CRYPTO_NUM settings and watching the console.

Each guest is booted without networking (this test needs none), so any number
of them can run at once; the panic lands at ~10 s.
"""
import argparse
import os
import shutil
import signal
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(REPO, "build", "qemu-system-mipsel")
IMG = os.path.join(REPO, "reinstalled.bin")


def boot(value, out, wait):
    os.makedirs(out, exist_ok=True)
    serial = os.path.join(out, "serial.log")
    for f in (serial,):
        try:
            os.unlink(f)
        except OSError:
            pass
    env = dict(os.environ)
    env.update({"MT7621_CMGCR": "1", "MT7621_CMGIC": "1", "MT7621_RX_LSO": "1"})
    if value is not None:
        env["MT7621_CRYPTO_NUM"] = hex(value)
    cmd = [QEMU, "-M", "mt7621,board=rb750gr3", "-m", "256M",
           "-kernel", IMG, "-display", "none", "-monitor", "none",
           "-serial", f"file:{serial}"]
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL, env=env)
    time.sleep(wait)
    if p.poll() is None:
        p.send_signal(signal.SIGINT)
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    try:
        with open(serial, "rb") as f:
            return f.read().decode("latin1", "replace")
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--values", default="0x00,0x93,0x97,0x196,0x197,0x95,0x99,"
                                        "0x28,0x36,0x56,0x82,0x92,0x94,0x98,"
                                        "0x01,0x60")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--wait", type=float, default=13.0)
    ap.add_argument("--out", default=os.path.join(REPO, "run", "sweep"))
    a = ap.parse_args()

    values = list(range(0x100)) if a.all else [
        int(v, 0) for v in a.values.split(",") if v.strip()]
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out, exist_ok=True)

    winners = []
    idx = 0
    batch = a.jobs
    while idx < len(values):
        chunk = values[idx:idx + batch]
        procs = []
        for v in chunk:
            out = os.path.join(a.out, "%02x" % v)
            p = subprocess.Popen([sys.executable, __file__, "--single",
                                  str(v), out, str(a.wait)],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
            procs.append((v, out, p))
        for v, out, p in procs:
            p.wait()
            text = ""
            try:
                text = open(os.path.join(out, "serial.log"),
                            "rb").read().decode("latin1", "replace")
            except OSError:
                pass
            panicked = "Kernel panic" in text
            state = "panic" if panicked else ("NO PANIC" if text else "no output")
            print("  0x%02x: %s" % (v, state), flush=True)
            if not panicked and text:
                winners.append(v)
        idx += batch

    print()
    if winners:
        print("accepted: " + ", ".join("0x%02x" % v for v in winners))
    else:
        print("no accepted value in this set")
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--single":
        boot(int(sys.argv[2], 0), sys.argv[3], float(sys.argv[4]))
        sys.exit(0)
    sys.exit(main())
