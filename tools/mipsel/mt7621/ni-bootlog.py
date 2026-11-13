#!/usr/bin/env python3
"""Boot a flash image for a few seconds and dump the guest's kernel log.

    tools/ni-bootlog.py                      stock RB750Gr3 image, 14 s
    tools/ni-bootlog.py --img foo.bin --wait 20
    tools/ni-bootlog.py --grep yaffs mtd     filter (default: yaffs mtd spi flash)

Why: the *installed* RouterOS kernel prints to the console only where it feels
like it (a panic shows up, the boot chatter does not), but its printk ring
buffer is always in RAM.  This boots the image, pmemsaves 256 MiB and pulls
the log out, which turns "did the filesystem mount?" into a ~40 second test.

The netboot kernel used by netinstall is silent on both UARTs, so this is the
quick way to iterate on the flash/MTD/yaffs layer.
"""
import argparse
import os
import signal
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULTS = {
    "qemu": os.path.join(REPO, "build", "qemu-system-mipsel"),
    "img": os.path.join(REPO, "RB750Gr3_6F3807CD31FE_704_r3.bin"),
    "out": os.path.join(REPO, "run", "boot"),
    "port": 4490,
}


def flagged(pattern):
    out = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True)
    return [int(p) for p in out.stdout.split()]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--img", default=DEFAULTS["img"])
    ap.add_argument("--qemu", default=DEFAULTS["qemu"])
    ap.add_argument("--out", default=DEFAULTS["out"])
    ap.add_argument("--port", type=int, default=DEFAULTS["port"])
    ap.add_argument("--wait", type=float, default=14.0, help="seconds before the dump")
    ap.add_argument("--grep", nargs="*", default=["yaffs", "mtd", "spi", "flash", "panic"])
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    serial = os.path.join(a.out, "serial.log")
    ram = os.path.join(a.out, "ram.bin")
    for f in (serial, ram):
        try:
            os.unlink(f)
        except OSError:
            pass

    env = dict(os.environ)
    env.update({"MT7621_CMGCR": "1", "MT7621_CMGIC": "1", "MT7621_RX_LSO": "1"})
    cmd = [a.qemu, "-M", "mt7621,board=rb750gr3", "-m", "256M",
           "-kernel", a.img, "-display", "none",
           "-monitor", f"unix:{os.path.join(a.out, 'monitor.sock')},server=on,wait=off",
           "-serial", f"file:{serial}"]
    qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=open(os.path.join(a.out, "qemu.err"), "wb"),
                            stdin=subprocess.DEVNULL, env=env)
    try:
        time.sleep(a.wait)
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(120)
        s.connect(os.path.join(a.out, "monitor.sock"))
        s.sendall(('pmemsave 0 268435456 "%s"\n' % ram).encode())
        time.sleep(25)
        s.close()
    finally:
        if qemu.poll() is None:
            qemu.send_signal(signal.SIGINT)
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()

    print("=== console (serial.log) ===")
    try:
        with open(serial, "rb") as f:
            text = f.read().decode("latin1")
        for line in [l.rstrip() for l in text.splitlines() if l.strip()][-15:]:
            print("  " + line)
    except OSError:
        print("  (none)")

    print("=== kernel log ===")
    klog = subprocess.run([os.path.join(REPO, "tools", "ni-klog.py"), ram] + a.grep,
                          capture_output=True, text=True)
    for line in klog.stdout.splitlines()[:60]:
        print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
