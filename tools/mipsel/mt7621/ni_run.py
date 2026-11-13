#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Drive a full netinstall run end to end, then check the flash boot.

    tools/ni_run.py                     one run
    tools/ni_run.py -n 5                five runs, stop early on success
    tools/ni_run.py -n 5 --keep-going   keep going after a success
    tools/ni_run.py -v                  add the chatty SPI traces
    tools/ni_run.py -q                  only the knobs the guest needs
    tools/ni_run.py --keep              leave the VM up on failure for poking

Artifacts land in run/NN/ (serial.log, qemu.err, netinstall.log, flash.bin,
flash.installed.bin).

The console is a telnet chardev: it is logged to serial.log (so this script can
follow it) and can be driven by hand while a run is in progress:

    telnet 127.0.0.1 4444

Installer privilege: netinstall-cli needs CAP_NET_RAW.  Either run this script
as root, or allow exactly that binary plus its pkill:

    amrelsaqqa ALL=(root) NOPASSWD: /home/amrelsaqqa/Downloads/netinstall-7.15/netinstall-cli
    amrelsaqqa ALL=(root) NOPASSWD: /usr/sbin/pkill -f netinstall-cli

Two phases:

  1. netinstall boot.  netinstall-cli is started *before* the guest so bootp
     already has a server; Ctrl+E at the "Press any key within 2 seconds to
     enter setup" window forces etherboot anyway, and Enter is sent whenever
     the booter parks on "Please, check ethernet cable..." because that is what
     restarts the attempt.  Keys are only ever pressed in this phase.
  2. After "Software Installed" (or the reboot prompt) the harness sends one
     Enter to reboot and then *stops typing* - the point is to observe the
     machine boot RouterOS from the NOR flash.  Failure to boot from flash
     (falls back to bootp, or "kernel not found or data is corrupted") is
     reported separately from an install failure.
"""
import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.expanduser("~")

DEFAULTS = {
    "qemu": os.path.join(REPO, "build", "qemu-system-mipsel"),
    "img": os.path.join(REPO, "rb750g3-dump-7.17.bin"),
    "cli": os.path.join(HOME, "Downloads/netinstall-7.15/netinstall-cli"),
    "package": os.environ.get("NI_PACKAGE", ""),
    "out": os.path.join(REPO, "run"),
    "iface": "tap0",
    "port": 4444,
}

# Knobs the guest needs.  RX_LSO is load-bearing: without it the installer's
# key exchange fails ("Key was rejected") and the guest never gets an offer.
BASE_KNOBS = ["MT7621_CMGCR=1", "MT7621_CMGIC=1", "MT7621_RX_LSO=1"]
TRACE_KNOBS = ["MT7621_RECVLOG=1", "MT7621_NILOG=1"]
SPI_KNOBS = ["MT7621_SYSLOG=1", "MT7621_RDLOG=1", "MT7621_WRLOG=1"]
RAW_SPI = ["MT7621_SPILOG=1"]   # register-level firehose, only via --spi

INSTALL_OK = ["Software Installed", "software installed",
              "installation completed", "installed successfully"]
INSTALL_FAIL = ["could not mount filesystem",
                "ERROR:", "installation failed", "Key was rejected"]
REBOOT_PROMPT = "Press ENTER to reboot"
# A flash boot shows "loading kernel..." and no bootp; RouterOS itself may stay
# silent (its cmdline carries no-uart), so these are best-effort markers.
FLASH_BOOT = ["loading kernel", "jumping to kernel code"]
ROUTEROS = ["RouterOS", "MikroTik", "login:", "Starting services",
            "system entered state"]


class Console:
    """Write bytes into the guest serial via the telnet chardev."""

    def __init__(self, port):
        self.port = port

    def send(self, data, settle=0.5):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        except OSError:
            return False
        try:
            s.sendall(data)
            time.sleep(settle)          # QEMU reads the chardev on its main loop
            s.sendall(data)             # first byte after connect is easy to lose
            time.sleep(settle)
        finally:
            s.close()
        return True


class Monitor:
    def __init__(self, path):
        self.path = path

    def cmd(self, command):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(10)
            s.connect(self.path)
        except OSError:
            return ""
        try:
            s.sendall((command + "\n").encode())
            out = b""
            deadline = time.time() + 5
            while time.time() < deadline:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    break
                out += chunk
            return out.decode("latin1", "replace")
        finally:
            s.close()


class Tail:
    """Follow a growing log file."""

    def __init__(self, path):
        self.path = path
        self.seen = ""
        self.pos = 0

    def text(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                chunk = f.read()
                self.pos = f.tell()
        except OSError:
            return self.seen
        if chunk:
            self.seen += chunk.decode("latin1", "replace")
        return self.seen

    def wait_for(self, needle, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            if needle in self.text():
                return True
            time.sleep(0.2)
        return False


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def port_busy(port):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=0.3)
        s.close()
        return True
    except OSError:
        return False


class Runner:
    def __init__(self, args):
        self.a = args
        self.sudo = ["sudo", "-n"] if os.geteuid() != 0 else []
        self.qemu = None
        self.cli = None

    # -- plumbing -----------------------------------------------------------

    def check(self):
        a = self.a
        for path, what in ((a.qemu, "emulator"), (a.cli, "installer"),
                           (a.package, "package")):
            if not os.path.exists(path):
                sys.exit(f"ni_run: no {what} at {path}")
        if self.sudo and sh(self.sudo + ["-l", a.cli]).returncode != 0:
            sys.exit(f"ni_run: sudoers does not allow {a.cli}")

    def stop_installers(self):
        # A bare pkill is the reason this harness is python: the pattern must
        # not appear in our own command line, and here it does not.
        sh(self.sudo + ["/usr/sbin/pkill", "-f", "netinstall-cli"])

    def stop_qemu(self):
        if self.qemu and self.qemu.poll() is None:
            self.qemu.send_signal(signal.SIGINT)
            try:
                self.qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.qemu.kill()
        self.qemu = None

    def cleanup_stale(self):
        self.stop_installers()
        # Kill every qemu for this machine, including one left behind by a
        # --keep run.  Match on the machine string: it only ever appears in a
        # qemu command line, never in this harness' own argv.
        try:
            pids = sh(["pgrep", "-f", "mt7621,board=rb750gr3"]).stdout.split()
        except OSError:
            pids = []
        for pid in pids:
            try:
                os.kill(int(pid), signal.SIGTERM)
            except (OSError, ValueError):
                pass
        if os.path.isdir(self.a.out):
            for name in os.listdir(self.a.out):
                pidfile = os.path.join(self.a.out, name, "qemu.pid")
                if os.path.exists(pidfile):
                    try:
                        os.kill(int(open(pidfile).read().strip()), signal.SIGTERM)
                    except (OSError, ValueError):
                        pass
                    os.unlink(pidfile)
        for _ in range(20):
            if not port_busy(self.a.port):
                return
            out = sh(["ss", "-ltnp"]).stdout
            m = re.search(r":%d\b.*?pid=(\d+)" % self.a.port, out)
            if m:
                try:
                    os.kill(int(m.group(1)), signal.SIGTERM)
                except OSError:
                    pass
            time.sleep(0.3)

    # -- processes ----------------------------------------------------------

    def start_installer(self, cur):
        log = open(os.path.join(cur, "netinstall.log"), "wb")
        self.cli = subprocess.Popen(
            self.sudo + [self.a.cli, "-i", self.a.iface] +
            (os.environ.get("NI_ARGS", "").split() if os.environ.get("NI_ARGS") else []) +
            [self.a.package],
            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)

    def start_qemu(self, cur):
        knobs = list(BASE_KNOBS)
        if not self.a.quiet:
            knobs += TRACE_KNOBS
        if self.a.verbose:
            knobs += SPI_KNOBS
        if self.a.spi:
            knobs += RAW_SPI
        knobs.append("MT7621_FLASH_SAVE=" + os.path.join(cur, "flash.bin"))
        env = dict(os.environ)
        for knob in knobs:
            k, v = knob.split("=", 1)
            env[k] = v
        serial = (f"socket,id=ser0,host=127.0.0.1,port={self.a.port},"
                  f"server=on,wait=off,telnet=on,"
                  f"logfile={os.path.join(cur, 'serial.log')}")
        cmd = [self.a.qemu, "-M", "mt7621,board=rb750gr3", "-m", "256M",
               "-kernel", self.a.img, "-display", "none",
               "-monitor", f"unix:{os.path.join(cur, 'monitor.sock')},server=on,wait=off",
               "-chardev", serial, "-serial", "chardev:ser0",
               "-netdev", f"tap,id=net0,ifname={self.a.iface},script=no,downscript=no"]
        self.qemu = subprocess.Popen(
            cmd, stdout=open(os.path.join(cur, "qemu.out"), "wb"),
            stderr=open(os.path.join(cur, "qemu.err"), "wb"),
            stdin=subprocess.DEVNULL, env=env)
        with open(os.path.join(cur, "qemu.pid"), "w") as f:
            f.write(str(self.qemu.pid))

    # -- phases -------------------------------------------------------------

    def installer_ready(self, cur, seconds=10):
        tail = Tail(os.path.join(cur, "netinstall.log"))
        deadline = time.time() + seconds
        while time.time() < deadline:
            if "Waiting for RouterBOARD" in tail.text():
                return True
            time.sleep(0.2)
        return False

    def boot_netinstall(self, log, console, mon, cur):
        """Phase 1a: get RouterBOOT to netboot.  Only place keys are pressed."""
        if not log.wait_for("Press Ctrl+E", 40):
            return False
        if log.wait_for("enter setup", 20):
            time.sleep(0.3)
            console.send(b"\x05", settle=0.4)          # Ctrl+E -> etherboot

        # The booter parks on "Please, check ethernet cable..."; a plain Enter
        # restarts the bootp attempt.  Keep pressing it while that prompt is the
        # last thing on the console - nothing else is needed here.
        deadline = time.time() + 180
        while time.time() < deadline:
            text = log.text()
            if "jumping to kernel code" in text:
                return True
            if text.rstrip().endswith("Please, check ethernet cable..."):
                console.send(b"\r", settle=0.5)
                time.sleep(0.4)
                continue
            if "kernel not found" in text and text.rstrip().endswith(
                    "corrupted"):
                console.send(b"\r", settle=0.5)
                time.sleep(0.4)
                continue
            time.sleep(0.15)
        return "jumping to kernel code" in log.text()

    def wait_install(self, log, cur):
        """Phase 1b: watch the UI until it reports success/failure."""
        cli = Tail(os.path.join(cur, "netinstall.log"))
        dumped = False
        deadline = time.time() + self.a.timeout
        while time.time() < deadline:
            text = log.text()
            if self.a.dump_at and not dumped and self.a.dump_at in text:
                dumped = True
                self.dump_ram(cur)
            # The installer side is authoritative once it has sent the package:
            # the UI may still be drawing its progress bar when netinstall-cli
            # finishes and exits, which used to be misread as a failure.
            clitxt = cli.text()
            if "Sent reboot command" in clitxt or "Ready for reboot" in clitxt:
                self.snapshot_flash(cur)
                return True, "installer finished (Ready for reboot)"

            for marker in INSTALL_FAIL:
                if marker in text:
                    return False, marker
            for marker in INSTALL_OK:
                if marker in text:
                    self.snapshot_flash(cur)
                    return True, marker
            if self.cli.poll() is not None:
                if "Sent reboot command" in cli.text():
                    self.snapshot_flash(cur)
                    return True, "installer finished"
                return False, "installer exited without finishing"
            if self.qemu.poll() is not None:
                return False, "guest died"
            time.sleep(0.3)
        return False, "timeout, no terminal state"

    def check_flash_boot(self, log, console):
        """Phase 2: reboot with one Enter, then do not touch anything."""
        if REBOOT_PROMPT in log.text():
            console.send(b"\r", settle=0.3)
        deadline = time.time() + self.a.timeout
        saw_bootp = saw_kernel_bad = False
        while time.time() < deadline:
            text = log.text()
            # everything after the reboot prompt is the second boot
            idx = text.rfind(REBOOT_PROMPT)
            second = text[idx:] if idx >= 0 else text
            if "trying bootp" in second:
                saw_bootp = True
            if "kernel not found or data is corrupted" in second:
                saw_kernel_bad = True
            if any(m in second for m in ROUTEROS):
                return True, "RouterOS booted from flash"
            if "Press Ctrl+E" in second and "kernel not found" in second:
                return False, "second boot found no kernel"
            if time.time() > deadline - 1:
                break
            time.sleep(0.3)
        text = log.text()
        idx = text.rfind(REBOOT_PROMPT)
        second = text[idx:] if idx >= 0 else ""
        if saw_bootp:
            return False, "second boot fell back to bootp (flash kernel unusable)"
        if saw_kernel_bad:
            return False, "second boot: kernel not found or corrupted"
        if "loading kernel" in second:
            return True, "flash kernel loaded (RouterOS console silent)"
        return False, "second boot: no evidence of a flash boot"

    @staticmethod
    def snapshot_flash(cur):
        src = os.path.join(cur, "flash.bin")
        dst = os.path.join(cur, "flash.installed.bin")
        for _ in range(20):
            try:
                with open(src, "rb") as f:
                    data = f.read()
                if len(data) == 16 << 20 and data.count(0xff) != len(data):
                    with open(dst, "wb") as f:
                        f.write(data)
                    return
            except OSError:
                pass
            time.sleep(0.5)

    # -- one run ------------------------------------------------------------

    def dump_ram(self, cur):
        """pmemsave the guest so tools/ni-klog.py can read the kernel log."""
        mon = Monitor(os.path.join(cur, "monitor.sock"))
        mon.cmd('pmemsave 0 268435456 "%s"' % os.path.join(cur, "ram.bin"))

    def run_once(self, cur):
        console = Console(self.a.port)
        mon = Monitor(os.path.join(cur, "monitor.sock"))
        # Installer FIRST: netinstall-cli parks in "Wait for Link-UP" and is
        # woken by the carrier-up event that QEMU's tap attach raises.  Started
        # *after* the guest is attached it waits forever, because that event has
        # already gone by - which is exactly how a run looks when bootp gets no
        # answer although `ip link` shows the tap up with carrier.
        self.start_installer(cur)
        time.sleep(1.0)
        self.start_qemu(cur)

        # netinstall-cli waits for the tap's carrier and, if the carrier was
        # already up when it started, it can miss the event and wait forever.
        # Restarting it once the guest is attached is free: a fresh process
        # sees the carrier immediately.
        for _ in range(3):
            if self.installer_ready(cur, 8):
                break
            if self.cli and self.cli.poll() is None:
                self.cli.kill()
            self.start_installer(cur)

        log = Tail(os.path.join(cur, "serial.log"))

        if not self.boot_netinstall(log, console, mon, cur):
            return False, "guest never netbooted", log.text()
        ok, why = self.wait_install(log, cur)
        if not ok:
            return False, why, log.text()

        boot_ok, boot_why = self.check_flash_boot(log, console)
        return boot_ok, boot_why, log.text()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", "--runs", type=int, default=1)
    ap.add_argument("-t", "--timeout", type=int, default=240,
                    help="seconds per phase (default 240)")
    ap.add_argument("-o", "--out", default=DEFAULTS["out"])
    ap.add_argument("--img", default=DEFAULTS["img"])
    # env fallbacks: the CLI path must not appear in our own argv, or the
    # cleanup pkill below would match this very process and kill it.
    ap.add_argument("--cli", default=os.environ.get("NI_CLI") or DEFAULTS["cli"])
    ap.add_argument("--package", default=os.environ.get("NI_PACKAGE") or DEFAULTS["package"])
    ap.add_argument("--qemu", default=DEFAULTS["qemu"])
    ap.add_argument("--iface", default=DEFAULTS["iface"])
    ap.add_argument("--port", type=int, default=DEFAULTS["port"])
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="no RECVLOG/NILOG (smaller logs)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="add SYSLOG/RDLOG/WRLOG (semantic flash + syscall trace)")
    ap.add_argument("--spi", action="store_true",
                    help="add SPILOG (raw register trace; huge)")
    ap.add_argument("--dump-at", default="",
                    help="pmemsave the guest as soon as this text appears on the console")
    ap.add_argument("--ram", action="store_true",
                    help="save the full 256 MiB guest RAM on failure (kernel log lives there)")
    ap.add_argument("--keep", action="store_true", help="leave the VM up on failure")
    ap.add_argument("--keep-going", action="store_true",
                    help="do not stop after the first success")
    a = ap.parse_args()

    runner = Runner(a)
    runner.check()
    os.makedirs(a.out, exist_ok=True)

    rc = 1
    for i in range(1, a.runs + 1):
        cur = os.path.join(a.out, f"{i:02d}")
        shutil.rmtree(cur, ignore_errors=True)
        os.makedirs(cur)
        print(f"=== run {i}/{a.runs} -> {cur}", flush=True)

        runner.cleanup_stale()
        ok, why, text = runner.run_once(cur)
        if not ok and a.ram and runner.qemu and runner.qemu.poll() is None:
            runner.dump_ram(cur)
        rc = 0 if ok else 1

        print("--- guest console tail")
        for line in [l for l in text.splitlines() if l.strip()][-12:]:
            print("   " + line)
        print(f"--- RESULT: {'SUCCESS' if ok else 'FAIL'} - {why}")
        print(f"--- artifacts: {cur}")

        if ok and not a.keep_going:
            print("=== stopping after first success")
            runner.stop_qemu()
            runner.stop_installers()
            return 0
        if not ok and a.keep:
            print("=== leaving VM running for inspection")
            runner.stop_installers()
            return rc
        runner.stop_qemu()
        runner.stop_installers()

    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
