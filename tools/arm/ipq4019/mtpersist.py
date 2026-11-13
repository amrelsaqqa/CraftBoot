#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""mtpersist.py -- prove MikroTik's flash program and erase survive a power cut.

RouterOS boots from a ramdisk (``root=/dev/ram0``) but mounts yaffs on
``mtdblock0``, which is the SPI NOR die behind the QUP this machine models, so a
file written under that mount is real flash traffic: WREN, page program, status
polls and, when the yaffs log needs room, sector erase.  The die lives in host
memory, so the only way a write can survive is for the model to have mirrored it
into the persistent die file.

A "manual reset" here is a genuine power cycle: QEMU is killed, a new QEMU is
started, and nothing is carried over except the file the die was told to keep
(``nor-file=``).  No gdb ``dump binary memory`` / ``restore``, no
monitor state: if the write only existed in RAM, boot 2 cannot see it, because
boot 2 is a different process.  The dump itself is opened read-only by the
machine and is never written back, which the run checks by hashing it.

  1. boot 1: write a 16 KiB pseudorandom payload and a text marker on /flash,
     ``sync``, hash them, kill QEMU (power cut);
  2. boot 2: new QEMU on the same die file, cold-boot, hash the file again --
     it has to be identical, and the marker has to read back;
  3. delete both, ``sync``, kill QEMU again;
  4. boot 3: new QEMU on the same die file, cold-boot, confirm the files are
     gone: the erase path persisted too.

Console input goes over the chardev socket while output is read from QEMU's own
UART log file: a socket client can stop receiving mid-session, and the log keeps
every byte.

Usage: ./mtpersist.py [dump] [--keep] [--drive]
"""
import hashlib
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.path.join(HERE, "build", "qemu-system-arm")
DEFAULT_DUMP = os.path.expanduser("~/mikrotik-dump-latest7.17-32m.bin")
PORT = 4445
BYPASS = "0xa8d1918:0x80000000"   # QSEE handover -> APPSBL entry
WORK = "/tmp/mtpersist"

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

GDB_AGENT = """set pagination off
set confirm off
target remote {sock}
hbreak *{bp}
commands 1
silent
printf "bypass: pc=%p lr=%p r0=%p cpsr=%p\\n", $pc, $lr, $r0, $cpsr
set $pc = {entry}
set $r0 = 0
set $cpsr = $cpsr & ~0x20
continue
end
continue
"""


def say(text):
    print("== %s" % text, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Machine:
    """One power cycle: a fresh QEMU process on the same dump and die file."""

    def __init__(self, tag, dump, persist, drive=False):
        self.tag = tag
        self.dump = dump
        self.persist = persist
        self.drive = drive
        self.console = os.path.join(WORK, "%s.uart" % tag)
        self.hostlog = os.path.join(WORK, "%s.host" % tag)
        self.glog = os.path.join(WORK, "%s.gdb" % tag)
        self.sock = os.path.join(WORK, "uart.sock")
        self.mon = os.path.join(WORK, "mon.sock")
        self.gdb = os.path.join(WORK, "gdb.sock")
        self.proc = None
        self.agent = None
        self.uart = None

    # -- console ---------------------------------------------------------
    def log_size(self):
        try:
            return os.path.getsize(self.console)
        except OSError:
            return 0

    def text(self, since=0):
        try:
            with open(self.console, "rb") as f:
                f.seek(since)
                data = f.read()
        except OSError:
            return ""
        return ANSI.sub(b"", data).replace(b"\r", b"").decode("utf-8",
                                                             "replace")

    def wait(self, seconds, want, since=0):
        end = time.time() + seconds
        while time.time() < end:
            if want in self.text(since):
                return True
            time.sleep(0.25)
        return False

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        try:
            self.uart.sendall(data)
        except OSError:
            pass

    def run(self, cmd, marker, seconds=900):
        # The marker must not appear literally in the echoed command line, or
        # the match fires on the echo instead of on the guest's output: the
        # shell expands ${M}, so only the real echo produces __MARKER__.
        since = self.log_size()
        self.send("%s; M=__%s; echo ${M}__\r" % (cmd, marker))
        ok = self.wait(seconds, "__%s__" % marker, since)
        body = self.text(since)
        start = body.find(cmd)
        start = 0 if start < 0 else start + len(cmd)
        end = body.find("__%s__" % marker)
        return ok, body[start:end if end >= 0 else None]

    def login(self):
        if not self.wait(480, "Login:"):
            return False
        since = self.log_size()
        self.send("devel\r")
        if not self.wait(90, "Password:", since):
            return False
        since = self.log_size()
        self.send("\r")
        return self.wait(240, "# ", since)

    # -- lifecycle -------------------------------------------------------
    def start(self):
        for p in (self.sock, self.mon, self.gdb, self.console):
            try:
                os.unlink(p)
            except OSError:
                pass
        open(self.hostlog, "w").close()
        open(self.glog, "w").close()

        env = dict(os.environ,
                   QEMU_IPQ_DIE_LOG=os.path.join(WORK, "%s.die" % self.tag),
                   QEMU_IPQ_SPI_LOG="1", QEMU_IPQ_PSHOLD_LOG="1")
        #
        # Two ways to hand the machine its die, and the point is that they are
        # the same die: nor-file= is a path the machine opens, nor-drive= is a
        # block backend somebody else opened (which is what libvirt's <disk>
        # becomes).  --drive exercises the second, so the persistence proof
        # covers the block layer and not just the file path.
        #
        machine = "ipq4019"
        die_args = []
        if self.drive:
            machine += ",nor-drive=nor"
            die_args = ["-drive", "if=none,id=nor,file=%s,format=raw"
                        % self.persist]
        else:
            machine += ",nor-file=%s" % self.persist
        host = open(self.hostlog, "wb")
        self.proc = subprocess.Popen([
            QEMU, "-M", machine, "-kernel", self.dump, "-display", "none"] +
            die_args + [
            "-monitor", "unix:%s,server=on,wait=off" % self.mon,
            "-d", "unimp", "-D", os.path.join(WORK, "%s.qemu" % self.tag),
            "-chardev", "socket,id=con,host=127.0.0.1,port=%d,server=on,"
                        "wait=off,logfile=%s,logappend=on"
                        % (PORT, self.console),
            "-serial", "chardev:con", "-S",
            "-gdb", "unix:%s,server=on,wait=off" % self.gdb,
        ], stdout=host, stderr=subprocess.STDOUT, env=env)

        for _ in range(600):
            try:
                if "boot-info handed" in open(self.hostlog,
                                              errors="replace").read():
                    break
            except OSError:
                pass
            time.sleep(0.05)
        else:
            say("%s: the machine never initialised" % self.tag)
            return False

        for _ in range(120):
            try:
                self.uart = socket.create_connection(("127.0.0.1", PORT),
                                                     timeout=2)
                self.uart.settimeout(0.3)
                break
            except OSError:
                time.sleep(0.1)
        else:
            say("%s: no UART socket" % self.tag)
            return False

        bp, entry = BYPASS.split(":")
        cmds = os.path.join(WORK, "%s.gcmds" % self.tag)
        with open(cmds, "w") as f:
            f.write(GDB_AGENT.format(sock=self.gdb, bp=bp, entry=entry))
        self.agent = subprocess.Popen(["gdb", "-batch", "-x", cmds],
                                      stdout=open(self.glog, "wb"),
                                      stderr=subprocess.STDOUT)

        for _ in range(1200):
            if "bypass: pc=" in open(self.glog, errors="replace").read():
                break
            time.sleep(0.1)
        else:
            say("%s: the QSEE redirect never ran" % self.tag)
            return False
        return True

    def stop(self):
        """Power cut: SIGKILL the machine, keep nothing but the die file."""
        if self.uart is not None:
            try:
                self.uart.close()
            except OSError:
                pass
            self.uart = None
        if self.agent is not None:
            self.agent.kill()
            self.agent.wait()
            self.agent = None
        if self.proc is not None:
            self.proc.kill()
            self.proc.wait()
            self.proc = None
        time.sleep(1)
        return True


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dump = args[0] if args else DEFAULT_DUMP
    keep = "--keep" in sys.argv
    drive = "--drive" in sys.argv

    os.makedirs(WORK, exist_ok=True)
    for tag in ("a", "b", "c"):
        for ext in ("uart", "host", "gdb", "qemu", "die"):
            try:
                os.unlink(os.path.join(WORK, "%s.%s" % (tag, ext)))
            except OSError:
                pass

    die = os.path.join(WORK, "die.bin")
    try:
        os.unlink(die)
    except OSError:
        pass

    if drive:
        # A fresh volume is what libvirt would hand over: empty, then the
        # machine programs it from the image the way a factory part is.
        open(die, "wb").close()
    say("die backend: %s" % ("-drive if=none,id=nor (block backend)"
                             if drive else "nor-file= (a path)"))

    dump_sha = sha256(dump)
    say("dump: %s" % dump)
    say("dump sha256 at start: %s" % dump_sha)

    # ---- boot 1: write ------------------------------------------------
    m = Machine("a", dump, die, drive)
    if not m.start():
        return 1
    if not m.login():
        say("boot1: no shell")
        return 1
    say("boot1: writing 16 KiB + a marker on /flash, then sync")
    _, out = m.run(
        "cd /flash && dd if=/dev/urandom of=qemu-persist.bin bs=1024 count=16 "
        "2>&1 | tail -1; echo qemu-marker-42 > qemu-marker.txt; sync; "
        "md5sum qemu-persist.bin qemu-marker.txt", "WRITE")
    print((out or "").strip()[-400:])
    match = re.search(r"([0-9a-f]{32})\s+qemu-persist\.bin", out or "")
    before = match.group(1) if match else None
    if not before:
        say("boot1: the write produced no hash")
        return 1
    say("boot1: md5 %s" % before)
    say("boot1: power cut")
    m.stop()

    die_sha_1 = sha256(die)
    say("die file sha256 after boot 1: %s" % die_sha_1)
    wrote_to_disk = die_sha_1 != dump_sha
    say("PROGRAM/ONS-DISK: %s" % ("PASS" if wrote_to_disk else "FAIL"))

    # ---- boot 2: power cycle, read back -------------------------------
    m = Machine("b", dump, die, drive)
    if not m.start() or not m.login():
        return 1
    say("boot2: reading the file back after the power cycle")
    _, out = m.run(
        "cd /flash && ls -l qemu-persist.bin qemu-marker.txt; "
        "md5sum qemu-persist.bin qemu-marker.txt; cat qemu-marker.txt", "READ")
    print((out or "").strip()[-500:])
    match = re.search(r"([0-9a-f]{32})\s+qemu-persist\.bin", out or "")
    after = match.group(1) if match else None
    say("boot2: md5 %s" % after)
    persist = bool(after) and after == before
    marker = "qemu-marker-42" in (out or "")
    say("PROGRAM/PERSIST: %s   marker: %s"
        % ("PASS" if persist else "FAIL", "PASS" if marker else "FAIL"))

    say("boot2: deleting both (erase path), sync")
    _, out = m.run("cd /flash && rm -f qemu-persist.bin qemu-marker.txt; "
                   "sync; ls -l qemu-persist.bin qemu-marker.txt", "RM")
    print((out or "").strip()[-300:])
    say("boot2: power cut")
    m.stop()

    die_sha_2 = sha256(die)
    say("die file sha256 after boot 2: %s" % die_sha_2)
    say("die file changed by the delete: %s"
        % ("yes" if die_sha_2 != die_sha_1 else "no"))

    # ---- boot 3: power cycle, must be gone ----------------------------
    m = Machine("c", dump, die, drive)
    if not m.start() or not m.login():
        return 1
    say("boot3: the files must be gone")
    _, out = m.run("cd /flash && ls -l qemu-persist.bin qemu-marker.txt",
                   "GONE")
    print((out or "").strip()[-300:])
    gone = any(w in (out or "") for w in ("No such file", "not found",
                                          "cannot access"))
    say("ERASE/PERSIST: %s" % ("PASS" if gone else "FAIL"))
    say("boot3: power cut")
    m.stop()

    print("---- die-level activity ----")
    for tag in ("a", "b", "c"):
        log = os.path.join(WORK, "%s.qemu" % tag)
        dlog = os.path.join(WORK, "%s.die" % tag)
        if not os.path.exists(log):
            continue
        e = os.popen("grep -ac 'die: erase' %s" % log).read().strip()
        p = os.popen("grep -acE 'xact op=0x(02|12)' %s" % dlog).read().strip()
        print("boot %s: erase=%s page-program=%s" % (tag, e, p))
    print("---- dump unchanged ----")
    dump_sha_end = sha256(dump)
    print("dump sha256 at end:   %s" % dump_sha_end)
    untouched = dump_sha_end == dump_sha
    say("DUMP/UNTOUCHED: %s" % ("PASS" if untouched else "FAIL"))

    if not keep:
        try:
            os.unlink(die)
        except OSError:
            pass
    return 0 if (persist and marker and gone and wrote_to_disk and
                 untouched) else 1


if __name__ == "__main__":
    sys.exit(main())
