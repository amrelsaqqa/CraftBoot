#!/usr/bin/env python3
"""Drive the SPI NAND on chip select 1 through the QUP, without an image.

No dump in this tree declares a `spi-nand@1` node, so Linux in them never
touches the second die.  This harness is how the model gets exercised anyway:
the machine is started stopped (-S) with a gdb stub, and every SPI byte is one
write to the QUP output FIFO plus one read of the input FIFO, with the chip
selects driven as the vendor DTS describes them

    cs-gpios = <&tlmm 54 0>, <&tlmm 59 0>;

so it walks exactly the registers `drivers/spi/spi-qup.c` walks.

    ./nandtest.py                 functional pass, die is RAM
    ./nandtest.py --persist       same, with /tmp/nandtest/nand.bin as the part
    ./nandtest.py --fresh         delete that die file first (start erased)
    ./nandtest.py --persist --fresh   then:
    ./nandtest.py --verify        a NEW QEMU on that die file, reads only

--verify prints what a fresh boot finds, which is the persistence proof: page 0
must still hold what was programmed, page 128 must still be erased, and page 64
was never written.  Two processes, one file, nothing else crosses the boundary.
"""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = os.path.join(HERE, "build/qemu-system-arm")
IMG = os.path.join(os.path.expanduser("~"), "kt708-ipq-openwrt24.10.0-no-wifi.bin")
WORK = "/tmp/nandtest"
DIE = os.path.join(WORK, "nand.bin")

TLMM = 0x01000000
QUP_OUT = 0x078b5110
QUP_IN = 0x078b5218
NOR_PIN = 54
NAND_PIN = 59

PAGE = 2048
OOB = 64                                # MX35LF1GE4AB: 2048+64, 128 MiB
PPBLK = 64

cmds = []


def wr32(addr, val):
    cmds.append("set {unsigned int}%#x = %#x" % (addr, val))


def cs(pin, level):
    """Drive a chip select the way gpiolib does.

    The TLMM IO register is bit 0 = pad input, bit 1 = the output latch
    (pinctrl-msm's GPIO_OUT), so "high" is 0x2 and "low" is 0x0.  Both
    selects are GPIO_ACTIVE_HIGH in the DTS and the node does not set
    SPI_CS_HIGH, so the SPI core inverts: a select is driven LOW.
    """
    wr32(TLMM + pin * 0x1000 + 0x4, 2 if level else 0)


def cs_pinctrl(pin):
    """Release a select the way pinctrl's mux_cs `output-high` does.

    pinctrl-msm programs the pin with a read-modify-write of the IO register,
    whose reset value is all ones, so releasing a select lands on 0xffffffff
    rather than 0x2.
    """
    wr32(TLMM + pin * 0x1000 + 0x4, 0xffffffff)


def xfer(b):
    """Clock one byte out and read the byte the die shifted in."""
    cmds.append("set {unsigned int}%#x = %#x" % (QUP_OUT, b))
    cmds.append('printf "M=%%02x\\n", *(unsigned char*)%#x' % QUP_IN)


def xfer_all(bs):
    for b in bs:
        xfer(b)


def xfer_linux(b):
    """Clock one byte the way spi_qup_write_to_fifo() packs it.

    One SPI word carries one byte, and the driver puts it in the top lane:
    `word |= data << (BITS_PER_BYTE * (3 - i))`.
    """
    cmds.append("set {unsigned int}%#x = %#x" % (QUP_OUT, b << 24))
    cmds.append('printf "M=%%02x\\n", *(unsigned char*)%#x' % QUP_IN)


def xfer_all_linux(bs):
    for b in bs:
        xfer_linux(b)


def force_cs(asserted):
    """QUP_IO_CONTROL FORCE_CS, as the two drivers disagree about it.

    QCA's boot drivers *assert* CS0 by setting the bit; Linux's
    spi_qup_set_cs() sets it on deassert and clears it while a transfer runs.
    """
    wr32(0x078b5304, 0x801 if asserted else 0x001)


def note(msg):
    cmds.append('printf "NOTE %s\\n"' % msg)


class bus:
    """One transaction per chip-select assertion.

    Asserting means driving the pin low; releasing means letting it go high
    again.  The model keys off that level, so the harness has to produce it.
    """

    held = None

    @classmethod
    def select(cls, which):
        if cls.held is not None:
            cls.release()
        cs(NAND_PIN if which == "nand" else NOR_PIN, 0)
        cls.held = which

    @classmethod
    def release(cls):
        if cls.held is None:
            return
        cs(NAND_PIN if cls.held == "nand" else NOR_PIN, 1)
        cls.held = None


select = bus.select


def init_pins():
    bus.held = None
    for pin in (NOR_PIN, NAND_PIN):
        wr32(TLMM + pin * 0x1000, 0x200)          # function 0 (gpio), output
        cs_pinctrl(pin)                          # mux_cs: output-high


def nand_rdid():
    note("nand rdid")
    select("nand")
    xfer_all([0x9F, 0x00])       # opcode + the dummy byte Macronix wants
    xfer(0x00)                   # manufacturer
    xfer(0x00)                   # device


def nand_rdid_no_level_change():
    """A select write that changes nothing must still start a transaction.

    gpiolib takes the pin for every message, so the same level can be written
    twice in a row; the second write must not be ignored.
    """
    note("nand rdid after a select that changes nothing")
    select("nand")
    cs(NAND_PIN, 0)
    cs(NAND_PIN, 0)
    xfer_all([0x9F, 0x00])
    xfer(0x00)
    xfer(0x00)


def nand_rdid_kernel_sequence():
    """The netinstall kernel's spi-nand probe, register for register.

    spi-qup drives the cs-gpio (pin 59 low), then runs its PIO engine: one
    QUP_STATE RESET per transfer, and SPI_IO_CONTROL rewritten from
    spi_qup_set_cs() with the inverted FORCE_CS convention.  The transaction
    belongs to the gpio the whole way through, so FORCE_CS must not steal the
    bus or drop the command the die is holding.
    """
    note("nand rdid, kernel pio sequence")
    init_pins()
    cs(NAND_PIN, 0)                 # spi_set_cs(true) -> gpio asserted
    force_cs(False)                 # spi_qup_set_cs(val=true)
    wr32(0x078b5004, 0x4)           # QUP_STATE RESET, start of io_config
    force_cs(False)                 # io_config rewrites SPI_IO_CONTROL
    wr32(0x078b5004, 0x5)           # QUP_STATE RUN
    xfer_all_linux([0x9F])          # the command transfer
    wr32(0x078b5004, 0x4)           # RESET again, start of the read transfer
    force_cs(True)                  # spi_qup_set_cs(val=false) -- deassert
    wr32(0x078b5004, 0x5)
    xfer_all_linux([0x00, 0x00, 0x00])   # dummy cycles to shift the id out
    force_cs(False)
    cs(NAND_PIN, 1)                 # spi_set_cs(false) -> gpio released
    force_cs(True)


def nor_rdid_kernel_sequence():
    """The same dance for CS0, where FORCE_CS and the gpio agree."""
    note("nor rdid, kernel pio sequence")
    cs(NOR_PIN, 0)
    force_cs(False)
    wr32(0x078b5004, 0x4)
    xfer_all_linux([0x9F])
    wr32(0x078b5004, 0x4)
    force_cs(True)
    wr32(0x078b5004, 0x5)
    xfer_all_linux([0x00, 0x00, 0x00])
    cs(NOR_PIN, 1)


def nand_get_feature(reg):
    note("nand get feature %02x" % reg)
    select("nand")
    xfer_all([0x0F, reg])
    xfer(0x00)


def nand_set_feature(reg, val):
    note("nand set feature %02x=%02x" % (reg, val))
    select("nand")
    xfer_all([0x1F, reg, val])


def nand_wren():
    select("nand")
    xfer(0x06)


def nand_page_read(page):
    note("nand page read %d" % page)
    select("nand")
    xfer_all([0x13, (page >> 16) & 0xff, (page >> 8) & 0xff, page & 0xff])


def nand_read_cache(col, n, tag):
    """READ FROM CACHE 03h: column address, one dummy byte, then data.

    The dummy is a turnaround, not a data clock -- the die starts driving the
    cache at the byte after it (macronix.c declares ndummy=1 for 03h).
    """
    note("nand read cache col=%d n=%d (%s)" % (col, n, tag))
    select("nand")
    xfer_all([0x03, (col >> 8) & 0xff, col & 0xff, 0x00])
    for _ in range(n):
        xfer(0x00)


def nand_prog_load(col, data):
    note("nand prog load col=%d n=%d" % (col, len(data)))
    select("nand")
    xfer_all([0x02, (col >> 8) & 0xff, col & 0xff])
    for b in data:
        xfer(b)


def nand_prog_exec(page):
    note("nand prog exec %d" % page)
    select("nand")
    xfer_all([0x10, (page >> 16) & 0xff, (page >> 8) & 0xff, page & 0xff])


def nand_erase(block):
    # BLOCK ERASE takes the page row of the block's first page, not a block
    # index: nanddev_pos_to_row() = eraseblock * pages_per_eraseblock.
    row = block * PPBLK
    note("nand erase block %d (row %d)" % (block, row))
    select("nand")
    xfer_all([0xD8, (row >> 16) & 0xff, (row >> 8) & 0xff, row & 0xff])


def nor_rdid():
    note("nor rdid (cs0)")
    select("nor")
    xfer_all([0x9F])
    for _ in range(3):
        xfer(0x00)


def full_sequence():
    init_pins()
    nand_rdid()
    nand_rdid_no_level_change()
    nand_rdid_kernel_sequence()
    nor_rdid_kernel_sequence()
    nand_get_feature(0xC0)              # status: busy once, then idle
    nand_get_feature(0xC0)
    nand_set_feature(0xB0, 0x11)        # quad enable + on-die ECC enable
    nand_get_feature(0xB0)
    nand_get_feature(0xA0)              # block protection: none
    nand_page_read(0)
    nand_get_feature(0xC0)
    nand_read_cache(0, 16, "page 0 before write")

    nand_wren()
    nand_get_feature(0xC0)              # WEL set by WREN
    nand_prog_load(0, b"HELLO-NAND")
    nand_prog_exec(0)
    nand_get_feature(0xC0)

    nand_page_read(0)
    nand_get_feature(0xC0)
    nand_read_cache(0, 10, "page 0 after program")

    # the spare area is where the bad-block marker and the FTL keep state
    nand_wren()
    nand_prog_load(PAGE, b"\x00\x01\x02\x03\xfe\xff")
    nand_prog_exec(0)
    nand_page_read(0)
    nand_get_feature(0xC0)
    nand_read_cache(PAGE, 6, "spare area")

    # a page nobody touched must still be erased
    nand_page_read(64)
    nand_get_feature(0xC0)
    nand_read_cache(0, 4, "page 64")

    # erase durability: program a page, then erase its whole block
    nand_wren()
    nand_prog_load(0, b"ERASE-ME")
    nand_prog_exec(128)
    nand_page_read(128)
    nand_get_feature(0xC0)
    nand_read_cache(0, 8, "page 128 after program")
    nand_wren()
    nand_erase(2)                       # pages 128..191
    nand_get_feature(0xC0)
    nand_page_read(128)
    nand_get_feature(0xC0)
    nand_read_cache(0, 8, "page 128 after erase")

    nor_rdid()                          # CS0 must not care about any of this


def verify_sequence():
    """Read-only: what a fresh boot finds on the die file."""
    init_pins()
    nand_rdid()
    for page, tag in ((0, "page 0 (the program must have survived)"),
                      (64, "page 64 (never written)"),
                      (128, "page 128 (the erase must have survived)")):
        nand_page_read(page)
        nand_get_feature(0xC0)
        nand_read_cache(0, 12, tag)
    nor_rdid()


def run_once(env_extra):
    os.makedirs(WORK, exist_ok=True)
    env = dict(os.environ, QEMU_IPQ_FLASH_IMAGE=IMG, QEMU_IPQ_FLASH_JEDEC="ef4019")
    env.update(env_extra)
    host = os.path.join(WORK, "host.log")
    gsock = os.path.join(WORK, "gdb.sock")
    gpath = os.path.join(WORK, "gdb.cmd")
    for p in (gsock, gpath):
        try:
            os.unlink(p)
        except OSError:
            pass
    proc = subprocess.Popen([
        QEMU, "-M", "ipq4019", "-display", "none", "-serial", "null",
        "-monitor", "none", "-S", "-gdb",
        "unix:%s,server=on,wait=off" % gsock,
    ], stdout=open(host, "wb"), stderr=subprocess.STDOUT, env=env)
    for _ in range(200):
        if os.path.exists(gsock):
            break
        time.sleep(0.05)
    time.sleep(0.2)
    open(gpath, "w").write("set pagination off\nset confirm off\n"
                           "target remote %s\n" % gsock + "\n".join(cmds) +
                           "\ndetach\nquit\n")
    r = subprocess.run(["gdb", "-batch", "-x", gpath], capture_output=True,
                       text=True, timeout=300)
    proc.kill()
    proc.wait()
    return r.stdout, open(host, errors="replace").read()


def parse(text):
    """An ordered list of (command, bytes) pairs out of the printf stream."""
    res = []
    cur = None
    for l in text.splitlines():
        if l.startswith("NOTE "):
            cur = [l[5:], []]
            res.append(cur)
        elif l.startswith("M=") and cur is not None:
            cur[1].append(l[2:])
    return res


def main():
    argv = sys.argv[1:]
    die = DIE if ("--persist" in argv or "--verify" in argv) else None
    if "--fresh" in argv:
        try:
            os.unlink(DIE)
        except OSError:
            pass

    (verify_sequence if "--verify" in argv else full_sequence)()

    env = {"QEMU_IPQ_NAND": "1"}
    if die:
        env["QEMU_IPQ_NAND_PERSIST"] = die
    text, host = run_once(env)

    for line in host.splitlines():
        if line.startswith("ipq4019: nand"):
            print(line)
    for k, v in parse(text):
        print("%-52s %s" % (k, " ".join(v)))


if __name__ == "__main__":
    main()
