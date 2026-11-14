/*
 * Qualcomm IPQ4018/IPQ4019 (ipq40xx) bring-up machine
 *
 * Not a faithful SoC model: just enough of the address map to run a
 * boot ROM / RouterBOOT / bare-metal payload and see how far it gets.
 *
 * Mapped:
 *   0x00000000  boot ROM (loaded from -bios, 256 KiB window)
 *   0x00022000  PRNG            (stub)
 *   0x00040000  low SoC ID/strap window SBL1 probes (stub)
 *   0x0005c000  PBL boot-device descriptor block (see below)
 *   0x0005e000  PBL boot-config / device-present block (see below)
 *   0x004a1000  qca-gcnt        (32-bit free running counter)
 *   0x004a2000  qca-qtimer      (56-bit udelay counter, same time base)
 *   0x004ab000  pshold          (reset register: dropping it resets the board)
 *   0x01000000  TLMM            (stub, shadowed)
 *   0x01800000  GCC             (stub, shadowed)
 *   0x01940000  TCSR            (stub)
 *   0x40000000  PCIe0 DBI/ELBI  (stub, 64 KiB; 0x40100000 config, 4 KiB)
 *   0x07700000  audio (ADSS)    (stub, 128 KiB)
 *   0x078af000  BLSP1 UART0     (TX only, -> -serial)
 *   0x078b0000  BLSP1 UART1     (TX only, -> -serial)
 *   0x07884000  BLSP1 BAM       (descriptor ring DMA into the QUP FIFOs)
 *   0x078b5000  BLSP1 QUP SPI   (SPI master + NOR die)
 *   0x07980000  QPIC NAND       (stub, shadowed)
 *   0x08000000  DRAM            (-m)
 *   0x08600000  1 MiB OCIMEM    (RAM; SBL1/PBL shared handshake area)
 *   0x0b000000  QGIC2 / APCS   (stub, 16 MiB)
 *   0x0e000000  64 MiB "flash window" RAM: put a SPI dump here with
 *               -device loader,file=dump.bin,addr=0xe0000000,force-raw=on
 *
 * ELF boot stages:
 *   With -kernel pointing at a flash dump whose first image is an ARM32 ELF,
 *   the machine loads every PT_LOAD segment at p_paddr (like the PBL would),
 *   jumps to e_entry, and hands r0 = QEMU_IPQ_BOOTINFO (a synthesized
 *   "PBL shared data" blob) to the stage's entry point.
 *
 * Tunables (environment variables, read at each access):
 *   QEMU_IPQ_ENTRY       reset PC (default 0). e.g. 0x81000000 to start at
 *                        RouterBOOT's link address instead of the PBL.
 *   QEMU_IPQ_TRACE=1     log every stubbed MMIO access (-d unimp)
 *   QEMU_IPQ_STUB_READ   value returned by unwritten stub registers
 *                        (default 0xffffffff)
 *   QEMU_IPQ_UART_SR     value returned when reading UART SR (default 0x0f)
 *   QEMU_IPQ_UART_LEGACY=1  also emit bytes written to offset 0x00 (16550-ish
 *                        "DR" layout) - off by default because MR1 writes at
 *                        0x00 would print garbage
 *   QEMU_IPQ_QTIMER_HZ   0x4a2000 QTIMER tick rate used by udelay()
 *                        (default 48000000)
 *   QEMU_IPQ_GCNT_HZ     qca-gcnt sleep counter tick rate (default 32768)
 *   QEMU_IPQ_NO_PMU_FIX=1  do not set PMCR.E at reset
 *   QEMU_IPQ_NO_TB_FLUSH=1  keep the previous boot's translated code (the
 *                        model's I-cache) across a reset
 *   QEMU_IPQ_QSEE=run    do not intercept the SBL1 -> QSEE handover (default
 *                        is to resume at the APPSBL entry the boot image
 *                        names, with r0 = 0; see ipq_qsee_redirect())
 *   QEMU_IPQ_BOOTINFO    address of the synthesized PBL shared data handed to
 *                        SBL1 in r0 (default 0x08680000)
 *   QEMU_IPQ_BOOTCFG     value of the SoC boot-config word (0x5e02c, bits 9:0)
 *   QEMU_IPQ_DEVCFG      value of the boot-device descriptor words (0x5e030+)
 *   QEMU_IPQ_SPI_LOG=1   log the decoded SPI command stream (-d unimp)
 *   QEMU_IPQ_QUP_TRACE=1 log only QUP SPI + BLSP1 BAM accesses, each with the
 *                        guest PC that touched the register.  Same lines as
 *                        QEMU_IPQ_TRACE=2 for those two blocks, without the
 *                        hundreds of megabytes the rest of the SoC adds.
 *   QEMU_IPQ_FLASH_SIZE  SPI NOR die size in bytes, power of two
 *                        1 MiB..64 MiB (default 32 MiB)
 *   QEMU_IPQ_FLASH_JEDEC RDID bytes as hex, e.g. "ef4019" or "20ba19"
 *                        (default: Winbond "ef40" + log2(size))
 *   QEMU_IPQ_FLASH_PERSIST=<file> keep the die in this file: created as a copy
 *                        of the dump on the first run, loaded instead of the
 *                        dump afterwards, and written through on every erase
 *                        and program.  The dump itself is never modified.
 *   QEMU_IPQ_NAND=1      put a SPI NAND behind chip select 1 (the part on the
 *                        board: MX35LF1GE4AB, 132 MiB incl. spare); opt-in
 *                        because it costs that much host RAM
 *   QEMU_IPQ_NAND_PERSIST=<file> keep that die in this file: created erased on
 *                        the first run, loaded afterwards, written through on
 *                        every program and fsync'ed on every erase.  Its length
 *                        must be exactly pages * (page + spare).
 *   QEMU_IPQ_NAND_SIZE   die size in bytes, a whole number of erase blocks
 *                        (default 1024 blocks = 132 MiB including spare)
 *   QEMU_IPQ_NAND_ID     RDID as hex, manufacturer + device (default "c212")
 *   QEMU_IPQ_NAND_LOG=1  log NAND commands, page reads and erases
 *   QEMU_IPQ_CS_GPIO     TLMM pin driving NOR chip select (default 54)
 *   QEMU_IPQ_NAND_CS_GPIO  TLMM pin driving NAND chip select (default 59)
 *   QEMU_IPQ_BAM_VER     BAM version read back from +0x1000 (default 7)
 *   QEMU_IPQ_BAM_EVENT   PIPE_EVENT bits raised when a transfer completes
 *                        (default 0x100000)
 *   QEMU_IPQ_TIMER_DBG=1 dump architected timer state once a second
 *   QEMU_IPQ_PSCI        smc (default) / hvc / none: EL3 stand-in used for
 *                        guest smc instructions (see ipq4019_init)
 *
 * Status: SBL1 boots from the raw dump and hands over to QSEE.  It probes the
 * SPI NOR through the BLSP1 BAM, parses the partition table and the CDT, and
 * loads QSEE, SEC and APPSBL.  Nothing in the dump is decoded, repacked or
 * patched -- the images run as they are, and the same machine boots SBL1 from
 * unrelated builds (BOOT.BF.3.1.1-00096 and -00117 both reach "SBL1, End").
 * Where it stops is inside QSEE, which needs its own set of peripherals.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <elf.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qapi/qapi-types-block-core.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "chardev/char-fe.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/core/cpu.h"
#include "hw/loader.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "block/block-io.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/device_tree.h"
#include "system/runstate.h"
#include "exec/tb-flush.h"
#include "hw/intc/arm_gic.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "qapi/util.h"
#include "qapi/qapi-types-ui.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "cpu.h"
#include "net/net.h"
#include "net/checksum.h"
#include "net/eth.h"
#include <libfdt.h>
#include <sys/stat.h>
#include "ipq4019/ipq4019.h"

/* the machine owns one SoC and one board */
IPQ4019State *ipq;
IPQBoardState *ipq_board;




uint32_t env_u32(const char *name, uint32_t def)
{
    const char *s = getenv(name);

    if (!s || !*s) {
        return def;
    }
    return strtoul(s, NULL, 0);
}

bool env_true(const char *name)
{
    return env_u32(name, 0) != 0;
}

/*
 * QPIC tracing.  Gated by QEMU_IPQ_QPIC_LOG alone: the messages used to go
 * through qemu_log_mask(LOG_UNIMP), which needs `-d unimp` as well, so a run
 * started from lan1-bridge.sh (which passes no -d) logged nothing even with
 * the variable set.  Diagnostics that need a second, easy-to-forget flag are
 * diagnostics that are not there when they are needed.
 */
void qpic_log(const char *fmt, ...)
{
    va_list ap;

    if (!env_true("QEMU_IPQ_QPIC_LOG") &&
        !env_true("QEMU_IPQ_QPIC_PGM")) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}


/*
 * The machine's devices, in two groups.
 *
 * IPQ4019State is the SoC: the controllers that exist on the die regardless of
 * which board the die is soldered to.  IPQBoardState is everything wired to it
 * from outside, which is the part that must become configuration.  Both are
 * owned by ipq4019_init() and handed to the MMIO callbacks as their opaque
 * pointer; the file-scope pointers below exist only so the device models can
 * reach their siblings, and they go away as each part becomes a qdev device
 * with properties.
 */


/* ------------------------------------------------------------------ */
/* BLSP1 QUP (SPI master) with a SPI NOR die behind it                 */
/* ------------------------------------------------------------------ */

/*
 * boot_flash_trans_nand drives the NOR through BLSP1 QUP0 in SPI mode.  The
 * guest builds its own register map (qca_qup_init, 0xa8d6d88) and, with the
 * BAM flag cleared, talks to the FIFOs with plain loads and stores, so this
 * model only has to:
 *
 *   - shadow the configuration registers,
 *   - treat byte stores to the output FIFO as an SPI byte stream and decode
 *     opcode/address/dummy the way a NOR die would,
 *   - hand data back through the input FIFO, four SPI bytes per word read,
 *   - keep QUP_OPERATIONAL honest so the polled loops terminate.
 *
 * The die contents are the flash window itself, so anything the boot chain
 * reads here is exactly the byte the dump has at that offset.
 */
#define QUP_REG_CONFIG       0x000
#define QUP_REG_STATE        0x004
#define QUP_REG_IO_MODES     0x008
#define QUP_REG_SW_RESET     0x00c
#define QUP_REG_OPERATIONAL  0x018
#define QUP_REG_ERROR_FLAGS  0x01c
#define QUP_REG_OP_MASK      0x028
#define QUP_REG_HW_VERSION   0x030
#define QUP_REG_MX_OUTPUT    0x100
#define QUP_REG_MX_WRITE     0x150
#define QUP_REG_MX_INPUT     0x200
#define QUP_REG_MX_READ      0x208
#define QUP_REG_IO_CONTROL   0x304
#define QUP_OUT_FIFO         0x110
#define QUP_IN_FIFO          0x218

/* QUP_IO_CONTROL: the driver holds chip select with FORCE_CS (qca_qup_spi.h) */
#define QUP_IO_CONTROL_FORCE_CS 0x800

/* QUP_STATE values (QUP_STATE_MASK is the low two bits) */
#define QUP_STATE_RESET      0
#define QUP_STATE_RUN        1
#define QUP_STATE_CLEAR      2
#define QUP_STATE_PAUSE      3

/* QUP_IO_M_MODES: output mode in bits 11:10, input mode in 13:12 */
#define QUP_IO_M_MODE_FIFO   0
#define QUP_IO_M_MODE_BLOCK  1
#define QUP_IO_M_MODE_DMOV   2
#define QUP_IO_M_MODE_BAM    3

/* QUP_OPERATIONAL bits (spi-qup.c names) */
#define QUP_OP_OUT_FIFO_NOT_EMPTY 0x10
#define QUP_OP_IN_FIFO_NOT_EMPTY  0x20
#define QUP_OP_OUT_FIFO_FULL      0x40
#define QUP_OP_IN_FIFO_FULL       0x80
#define QUP_OP_OUT_SERVICE_FLAG   0x100
#define QUP_OP_IN_SERVICE_FLAG    0x200
#define QUP_OP_MAX_OUTPUT_DONE    0x400
#define QUP_OP_MAX_INPUT_DONE     0x800
#define QUP_OP_OUT_BLOCK_WRITE_REQ 0x1000
#define QUP_OP_IN_BLOCK_READ_REQ   0x2000



static uint32_t ipq_flash_die_size = IPQ_FLASH_DEFAULT;
static uint8_t ipq_flash_jedec[4] = { 0xef, 0x40, 0x19, 0x00 };
static int ipq_flash_jedec_len = 3;
static uint8_t ipq_flash_uid[8];
static int ipq_flash_uid_len = 8;

/*
 * QEMU_IPQ_DIE_LOG=1: a one-line summary per chip-select transaction, on
 * stderr (unbuffered, so a killed run still shows it).  This is how the
 * machine proves what the die actually shifted out for a given SPI command.
 * A value that looks like a path (it contains a '/') is opened as its own
 * file instead, which keeps the trace apart from the -D log.
 */
static uint8_t die_dbg_first[8];
static uint32_t die_dbg_n;

/*
 * Raw write(2) to a dedicated file: no stdio buffering and no dependence on
 * where QEMU has pointed stderr, so a run that is killed later still has the
 * whole trace on disk.
 */


void die_dbg_log(const char *fmt, ...)
{
    static int fd = -1;
    char buf[320];
    int n;
    va_list ap;

    if (fd < 0) {
        const char *path = getenv("QEMU_IPQ_DIE_LOG");

        if (!path || !*path) {
            fd = -2;
            return;
        }
        /*
         * A bare word ("1") means "on, to stderr"; only a value with a '/'
         * in it is a path.  Opening a file literally called "1" in the
         * current directory is what the old code did, and a guest that talks
         * to the die for a minute then left a 400 MB file nobody asked for.
         */
        if (!strchr(path, '/')) {
            fd = STDERR_FILENO;
            return;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fd = -2;
            return;
        }
    }
    if (fd < 0) {
        return;
    }
    va_start(ap, fmt);
    n = snprintf(buf, sizeof(buf), "[%9.3f] ",
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    if (n > 0 && n < (int)sizeof(buf)) {
        int m = vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);

        if (m > 0) {
            n += m;
        }
    }
    va_end(ap);
    if (n > 0) {
        ssize_t r = write(fd, buf, MIN((size_t)n, sizeof(buf) - 1));
        (void)r;
    }
}

/*
 * Mirror of what the guest writes to the UART, timestamped on the same clock
 * as the die transactions.  Without this there is no way to tell which flash
 * access belongs to which kernel message.
 */
void die_dbg_ser(uint8_t c)
{
    static char line[256];
    static unsigned n;

    if (!die_dbg_on()) {
        return;
    }
    if (c == '\r') {
        return;
    }
    if (c == '\n' || n >= sizeof(line) - 1) {
        line[n] = 0;
        if (n) {
            die_dbg_log("SER: %s\n", line);
        }
        n = 0;
        return;
    }
    line[n++] = c;
}

bool die_dbg_on(void)
{
    const char *v = getenv("QEMU_IPQ_DIE_LOG");

    /* "0"/"" mean off, like every other knob here; "1" and paths mean on. */
    return v && *v && strcmp(v, "0") != 0;
}

/*
 * The guest's PC for a log line.  current_cpu is NULL when the access comes
 * from the monitor, the gdb stub or a DMA engine rather than from a vCPU, and
 * a debug line must never be the thing that crashes the machine.
 */
uint32_t die_dbg_pc(void)
{
    return current_cpu ? (uint32_t)ARM_CPU(current_cpu)->env.regs[15] : 0;
}

static void die_dbg_note(uint8_t v)
{
    if (die_dbg_n < sizeof(die_dbg_first)) {
        die_dbg_first[die_dbg_n] = v;
    }
    die_dbg_n++;
}

static uint8_t qup_flash_byte(uint32_t off)
{
    if (!ipq_board->nor || off >= ipq_board->nor->size) {
        return 0xff;
    }
    return ipq_board->nor->data[off];
}

/* ---- NOR die write path ------------------------------------------------ */

#define NOR_PAGE_SIZE   256
#define NOR_SECTOR_SIZE 0x1000          /* 0x20: 4 KiB sector erase */
#define NOR_BLOCK32     0x8000          /* 0x52: 32 KiB block erase */
#define NOR_BLOCK64     0x10000         /* 0xd8: 64 KiB block erase */

/* SR2 failure flags (Winbond W25Q layout): the die reports a refused write */
#define NOR_SR2_EFAIL   0x80
#define NOR_SR2_PFAIL   0x40

enum {
    NOR_WR_NONE = 0,
    NOR_WR_PP,                          /* page program, 256 B */
    NOR_WR_SE4K,                        /* sector erase, 4 KiB */
    NOR_WR_BE32,                        /* block erase, 32 KiB */
    NOR_WR_BE64,                        /* block erase, 64 KiB */
    NOR_WR_CE,                          /* chip erase */
};

/*
 * Block protection as the die implements it: SR1 carries BP2..BP0 (bits 4:2),
 * TB (bit 5) selects the top or the bottom of the die, SEC (bit 6) protects
 * only the outermost 4 KiB sector, and BP=7 covers everything.  SR1 comes out
 * of reset as 0 and SBL1 clears a protect bit during init, so nothing is
 * protected in either dump -- but a bootloader that does protect a region has
 * to see the write refused, which a real die reports as P_FAIL/E_FAIL in SR2.
 */
static bool nor_range_protected(IPQQupl *q, uint32_t off, uint32_t len)
{
    uint32_t die = ipq_flash_die_size;
    uint32_t prot;
    bool top = (q->status & 0x20) != 0;
    unsigned bp = (q->status >> 2) & 7;

    if (q->status & 0x40) {                 /* SEC */
        prot = NOR_SECTOR_SIZE;
    } else if (!bp) {
        return false;
    } else {
        prot = (bp == 7) ? die : (NOR_SECTOR_SIZE << bp);
        prot = MIN(prot, die);
    }
    return top ? off + len > die - prot : off < prot;
}

/* ------------------------------------------------------------------ */
/* a die's backing store: a file, or a -drive backend                  */
/* ------------------------------------------------------------------ */

/*
 * A die is a device with storage behind it, and there are two ways to give it
 * storage, because there are two worlds to fit into:
 *
 *   -machine ipq4019,nor-file=board.bin          the machine opens the file
 *   -drive if=none,id=nor,file=board.bin,format=raw
 *   -machine ipq4019,nor-drive=nor               the machine uses that backend
 *
 * The second is what a libvirt <disk> produces: libvirt owns the file, its
 * format and its cache mode, and hands QEMU a block backend by name.  Both
 * reach the die through this struct, so the QUP code never asks which one it
 * has and the rest of the machine knows about neither.
 *
 * A store that cannot be written is still a valid die -- it is a part whose
 * write protect is on.  The guest gets P_FAIL/E_FAIL from the operations it
 * attempts, which is what silicon does, and the RAM image is left alone
 * instead of quietly diverging from what a power cycle would show.
 */
typedef struct IPQDieStore {
    int fd;                     /* nor-file= / nand-file=, -1 if absent */
    BlockBackend *blk;          /* nor-drive= / nand-drive=, NULL if absent */
    bool ro;                    /* the guest may not change this die */
    const char *what;           /* "nor" / "nand", for messages */
} IPQDieStore;

static bool ipq_die_store_open(IPQDieStore *s)
{
    return s->fd >= 0 || s->blk;
}

static bool ipq_die_store_writable(IPQDieStore *s)
{
    return ipq_die_store_open(s) && !s->ro;
}

static void ipq_die_store_fail(IPQDieStore *s, const char *op, uint64_t off)
{
    error_report("ipq4019: %s %s at 0x%" PRIx64 " failed", s->what, op, off);
}

/* The store's own length, or 0 when it does not have one to report. */
static uint64_t ipq_die_store_len(IPQDieStore *s)
{
    if (s->blk) {
        int64_t n = blk_getlength(s->blk);

        return n < 0 ? 0 : (uint64_t)n;
    }
    return 0;
}

/* Read len bytes at off into dst.  The length was validated when it was bound. */
static bool ipq_die_store_pread(IPQDieStore *s, void *dst, uint64_t len,
                                uint64_t off)
{
    if (s->blk) {
        return blk_pread(s->blk, off, len, dst, 0) == 0;
    }
    while (len) {
        ssize_t n = pread(s->fd, dst, len, off);

        if (n <= 0) {
            return false;
        }
        dst = (uint8_t *)dst + n;
        off += n;
        len -= n;
    }
    return true;
}

static void ipq_die_store_pwrite(IPQDieStore *s, const void *src, uint32_t len,
                                 uint32_t off)
{
    if (!ipq_die_store_writable(s)) {
        return;
    }
    if (s->blk) {
        if (blk_pwrite(s->blk, off, len, src, 0) < 0) {
            ipq_die_store_fail(s, "write", off);
        }
        return;
    }
    while (len) {
        ssize_t n = pwrite(s->fd, src, len, off);

        if (n <= 0) {
            ipq_die_store_fail(s, "write", off);
            close(s->fd);
            s->fd = -1;
            return;
        }
        src = (const uint8_t *)src + n;
        off += n;
        len -= n;
    }
}

/*
 * An erase is the operation a later read depends on most, so give it a real
 * durability point rather than relying on the page cache.  A block backend is
 * asked to flush too: its cache mode is libvirt's choice, and if libvirt chose
 * writeback then QEMU exiting is not a power cut until the flush says so.
 */
static void ipq_die_store_sync(IPQDieStore *s)
{
    if (s->fd >= 0 && !s->ro) {
        fsync(s->fd);
    } else if (s->blk && !s->ro) {
        blk_flush(s->blk);
    }
}

/*
 * Bind a die to a store: a path the machine opens, or the name of a block
 * backend somebody else opened.  Returns the store's length, or 0 when the
 * store is empty and the caller should program it from the board's image.
 *
 * `exact` is for a part whose length *is* its geometry (a NAND die file
 * carries the spare area, so any other length is a different part); without
 * it the store may be larger than the die and the extra is simply never
 * addressed, which is what a dump image covering less than the part looks
 * like.
 */
static uint64_t ipq_die_store_bind(IPQDieStore *s, const char *path,
                                   const char *node, uint64_t want, bool exact)
{
    struct stat st;
    uint64_t len;

    if (path && node) {
        error_report("ipq4019: %s-file= and %s-drive= are two ways to say "
                     "the same thing; give one", s->what, s->what);
        exit(1);
    }
    if (path) {
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);

        if (fd < 0) {
            error_report("ipq4019: %s-file=%s: %s", s->what, path,
                         strerror(errno));
            exit(1);
        }
        if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
            error_report("ipq4019: %s-file=%s is not a regular file",
                         s->what, path);
            exit(1);
        }
        s->fd = fd;
        s->ro = false;
        len = st.st_size;
        if (!len) {
            return 0;
        }
    } else {
        Error *err = NULL;

        s->blk = blk_by_name(node);
        if (!s->blk) {
            error_report("ipq4019: %s-drive=%s: no block backend with that "
                         "id; add -drive if=none,id=%s,file=...", s->what,
                         node, node);
            exit(1);
        }
        /*
         * A -drive backend starts with no permissions requested, because a
         * device model normally claims it and asks for what it needs.  This
         * machine has no block device to attach: the die is the consumer, so
         * ask here.  That is also what upgrades an auto-read-only image,
         * which is how a raw file is opened until somebody says it will be
         * written.  Being refused is not fatal -- the die is then a
         * write-protected part.
         */
        /*
         * RESIZE is part of it because an empty backend has to be grown into a
         * die: the block layer refuses a write past the end of an image unless
         * the writer is allowed to change its length.
         */
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE |
                         BLK_PERM_RESIZE,
                         BLK_PERM_ALL & ~(BLK_PERM_WRITE | BLK_PERM_RESIZE),
                         &err)) {
            s->ro = true;
            fprintf(stderr, "ipq4019: %s drive %s is read-only (%s): the die "
                    "will refuse program and erase\n", s->what, node,
                    error_get_pretty(err));
            error_free(err);
        }
        len = ipq_die_store_len(s);
        if (!len) {
            /*
             * A raw file that did not exist yet is a zero-length backend, i.e.
             * an unprogrammed part: the caller fills it and snapshots, exactly
             * as it would for a newly created die file.  The block layer will
             * not write past the end of a zero-length image, so give the
             * backend the die's length first -- this is what a volume created
             * by `qemu-img create` (or libvirt) already has.
             */
            if (!s->ro && blk_truncate(s->blk, want, true, PREALLOC_MODE_OFF,
                                       0, &err)) {
                fprintf(stderr, "ipq4019: %s drive %s cannot be sized to %"
                        PRIu64 " bytes (%s)\n", s->what, node, want,
                        error_get_pretty(err));
                error_free(err);
            }
            return 0;
        }
    }

    if (exact ? len != want : len < want) {
        error_report("ipq4019: %s store holds %" PRIu64 " bytes, %s %" PRIu64
                     "; remove it to start from %s", s->what, len,
                     exact ? "but this die is exactly" : "less than the die's",
                     want, exact ? "an erased part" : "the board's image");
        exit(1);
    }
    return len;
}

/*
 * A host-created volume is not a die.  `qemu-img create -f raw die.img 32M`
 * -- which is what libvirt does before it hands QEMU a <disk> -- produces a
 * sparse file that reads as zeros, while a NOR part that has never been
 * programmed reads 0xff.  Treating those zeros as the board's flash would mean
 * a machine that cannot boot from a freshly created volume, so an all-zero
 * store is "nothing here yet", and only when the board also names an image to
 * program it with.  A die the guest wrote is never all zeros across its whole
 * length, and a store without an image is always taken as it is.
 */
static bool ipq_die_store_blank(const void *buf, uint64_t len)
{
    const uint8_t *p = buf;
    uint64_t i;

    for (i = 0; i < len; i++) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

/*
 * QEMU_IPQ_FLASH_PERSIST=<file> / nor-file=<file> turns the die into a real
 * device that keeps what the guest wrote after QEMU exits.
 *
 * The dump named on the command line stays a read-only factory image: it is
 * read into the die at power-on and never written back, so its checksum cannot
 * change.  The store is the die itself.  On the first run it is created as a
 * copy of what the die holds after the dump was loaded; on every later run it
 * is loaded instead of the dump.  Each erase and each programmed byte is then
 * mirrored into it, which is what makes a manual power cycle -- kill QEMU,
 * start a new one -- retain the write.
 *
 * One file per dump, one die per file: a store smaller than the configured die
 * is refused rather than extended, because a die that suddenly reads 0xff where
 * it used to hold data is worse than a loud failure.
 */
static IPQDieStore ipq_nor_store = { .fd = -1, .what = "nor" };

static void nor_persist(const void *src, uint32_t off, uint32_t len)
{
    uint32_t die = ipq_flash_die_size;

    if (off >= die) {
        return;
    }
    ipq_die_store_pwrite(&ipq_nor_store, src, MIN(len, die - off), off);
}

#define NOR_PERSIST_NONE   (-1)
#define NOR_PERSIST_EMPTY   0
#define NOR_PERSIST_LOADED  1

/*
 * Bind the NOR die to its store.  LOADED means the store already held a die --
 * then it has been read into the array and the caller must not load the image
 * on top of it, because the store *is* the board's flash.  EMPTY means the
 * store was just created, so the caller loads the image and calls
 * nor_persist_snapshot() to make that the initial contents.  NONE means no
 * store was asked for and the die is RAM only.
 */
static int nor_persist_preload(IPQMem *flash, uint32_t die, const char *img,
                               const char *path, const char *drive)
{
    struct stat st, ist;
    uint64_t len;

    if (!path && !drive) {
        return NOR_PERSIST_NONE;
    }
    if (path && img && !stat(img, &ist) &&
        !stat(path, &st) && ist.st_dev == st.st_dev &&
        ist.st_ino == st.st_ino) {
        /*
         * Naming the dump itself is allowed -- that is "the chip is the file"
         * -- but it stops being a factory image, so say so once and loudly.
         */
        warn_report("nor-file is the dump itself: guest writes will modify "
                    "%s", img);
    }
    len = ipq_die_store_bind(&ipq_nor_store, path, drive, die, false);
    if (!len) {
        /*
         * An empty store has to be programmable, because the only thing that
         * can make it a die is the image the board named.
         */
        if (!ipq_die_store_writable(&ipq_nor_store)) {
            error_report("ipq4019: nor store %s is empty and cannot be "
                         "written, so there is no die to boot from",
                         path ? path : drive);
            exit(1);
        }
        return NOR_PERSIST_EMPTY;
    }
    if (!ipq_die_store_pread(&ipq_nor_store, flash->data, die, 0)) {
        error_report("ipq4019: reading the %s store failed",
                     ipq_nor_store.what);
        exit(1);
    }
    if (img && ipq_die_store_blank(flash->data, die)) {
        fprintf(stderr, "ipq4019: nor store %s is blank (all 0x00): "
                "programming it from %s\n", path ? path : drive, img);
        return NOR_PERSIST_EMPTY;
    }
    fprintf(stderr, "ipq4019: nor store %s loaded (%u MiB)\n",
            path ? path : drive, (unsigned)(die / MiB));
    return NOR_PERSIST_LOADED;
}

/* The store did not exist yet: make it a copy of what the image produced. */
static void nor_persist_snapshot(uint32_t die)
{
    if (!ipq_die_store_open(&ipq_nor_store)) {
        return;
    }
    ipq_die_store_pwrite(&ipq_nor_store, ipq_board->nor->data, die, 0);
    ipq_die_store_sync(&ipq_nor_store);
    fprintf(stderr, "ipq4019: nor store %u MiB written (a copy of the image)\n",
            (unsigned)(die / MiB));
}

static void nor_erase(IPQQupl *q, uint32_t off, uint32_t len)
{
    if (!ipq_board->nor) {
        return;
    }
    off &= ipq_flash_die_size - 1;
    if (off >= ipq_board->nor->size) {
        return;
    }
    len = MIN(len, (uint32_t)(ipq_board->nor->size - off));
    /*
     * A store that cannot be written is a write-protected part, so the die
     * refuses the operation the way it refuses a protected range: E_FAIL in
     * SR2, contents unchanged.
     */
    if (nor_range_protected(q, off, len) ||
        (ipq_die_store_open(&ipq_nor_store) &&
         !ipq_die_store_writable(&ipq_nor_store))) {
        q->status2 |= NOR_SR2_EFAIL;
        return;
    }
    memset(ipq_board->nor->data + off, 0xff, len);
    nor_persist(ipq_board->nor->data + off, off, len);
    ipq_die_store_sync(&ipq_nor_store);
    if (env_true("QEMU_IPQ_SPI_LOG")) {
        qemu_log_mask(LOG_UNIMP, "die: erase 0x%06x +0x%x\n",
                      off, (unsigned)len);
    }
}

static void nor_program_byte(IPQQupl *q, uint8_t b)
{
    uint32_t page;

    if (!ipq_board->nor || !q->pgm_left || q->pgm_addr >= ipq_board->nor->size) {
        return;
    }
    if (nor_range_protected(q, q->pgm_addr, 1) ||
        (ipq_die_store_open(&ipq_nor_store) &&
         !ipq_die_store_writable(&ipq_nor_store))) {
        q->status2 |= NOR_SR2_PFAIL;
        return;
    }
    /* Programming can only clear bits; a set bit needs an erase first. */
    ipq_board->nor->data[q->pgm_addr] &= b;
    nor_persist(&ipq_board->nor->data[q->pgm_addr], q->pgm_addr, 1);
    q->pgm_left--;
    /* Past the 256-byte page the die wraps instead of spilling into the next */
    page = q->pgm_addr & ~(NOR_PAGE_SIZE - 1);
    q->pgm_addr = page | ((q->pgm_addr + 1) & (NOR_PAGE_SIZE - 1));
}

/* The address phase of a write command just finished: start the operation. */
static void nor_write_start(IPQQupl *q)
{
    uint32_t off = q->addr & (ipq_flash_die_size - 1);

    if (!(q->status & 0x02)) {
        /*
         * No write-enable latch: a real die ignores the command silently,
         * but say so, because a driver that forgot WREN is then the only
         * explanation for the data that did not change.
         */
        if (env_true("QEMU_IPQ_SPI_LOG")) {
            qemu_log_mask(LOG_UNIMP,
                          "die: op 0x%02x at 0x%06x without WEL, ignored\n",
                          q->opcode, off);
        }
        q->wr_kind = NOR_WR_NONE;
        return;
    }
    switch (q->wr_kind) {
    case NOR_WR_PP:
        q->pgm_addr = off;
        q->pgm_left = NOR_PAGE_SIZE - (off & (NOR_PAGE_SIZE - 1));
        break;
    case NOR_WR_SE4K:
        nor_erase(q, off, NOR_SECTOR_SIZE);
        break;
    case NOR_WR_BE32:
        nor_erase(q, off, NOR_BLOCK32);
        break;
    case NOR_WR_BE64:
        nor_erase(q, off, NOR_BLOCK64);
        break;
    case NOR_WR_CE:
        nor_erase(q, 0, ipq_flash_die_size);
        break;
    default:
        break;
    }
    if (q->wr_kind != NOR_WR_PP) {
        q->wr_kind = NOR_WR_NONE;
    }
    q->status &= ~0x02;             /* the latch is consumed by the operation */
    q->wip_pending = 1;             /* one "busy" status read is owed */
}

/* ------------------------------------------------------------------ */
/* SPI NAND behind the second chip select                              */
/* ------------------------------------------------------------------ */

/*
 * The board has two dies on the one QUP: the NOR behind chip select 0 (boot
 * flash, above) and a Macronix SPI NAND behind chip select 1.  The vendor DTS
 * describes the pair the way mainline spi-qup consumes it:
 *
 *     cs-gpios = <&tlmm 54 GPIO_ACTIVE_HIGH>, <&tlmm 59 GPIO_ACTIVE_HIGH>;
 *     spi-nand@1 { compatible = "spi-nand"; reg = <1>; };
 *
 * spi-qup sets host->use_gpio_descriptors, so spi core drives both selects
 * through gpiolib and never touches the QUP's FORCE_CS: the pin level, not a
 * controller register, is what tells the two dies apart.  The model therefore
 * watches TLMM pins 54 and 59 and treats any level change on either as the
 * start of a new transaction on that die.  That is polarity-free on purpose --
 * the vendor DTS marks both pins GPIO_ACTIVE_HIGH while every mainline board
 * writes GPIO_ACTIVE_LOW for CS0 -- and an edge brackets a transfer either way.
 *
 * Geometry is the part on the board, the MX35LF1GE4AB: 1 Gbit, 2048-byte pages
 * with 64 bytes of spare, 64 pages in a 128 KiB erase block, 1024 blocks.  The spare area is part of the die and part of the die
 * file, because that is where the bad-block markers live and where the FTL
 * keeps its erase counters and volume headers.  On-die ECC is modelled as
 * "nothing to correct, nothing to report": the status register's ECC bits say
 * a page came back clean, which is what a fresh part does.
 */
#define NAND_CS_NOR   0
#define NAND_CS_NAND  1

/* SPI NAND opcodes (mainline drivers/mtd/nand/spi/) */
#define NAND_OP_RESET        0xff
#define NAND_OP_RDID         0x9f
#define NAND_OP_GET_FEATURE  0x0f
#define NAND_OP_SET_FEATURE  0x1f
#define NAND_OP_WREN         0x06
#define NAND_OP_WRDI         0x04
#define NAND_OP_PAGE_READ    0x13
#define NAND_OP_READ_CACHE   0x03    /* x1, no dummy */
#define NAND_OP_READ_CACHE_F 0x0b    /* x1 fast, 1 dummy byte */
#define NAND_OP_READ_CACHE_2 0x3b    /* x2, 1 dummy byte */
#define NAND_OP_READ_CACHE_4 0x6b    /* x4, 1 dummy byte */
#define NAND_OP_PROG_LOAD    0x02
#define NAND_OP_PROG_LOAD_R  0x84    /* same, column not reset */
#define NAND_OP_PROG_LOAD_4  0x32
#define NAND_OP_PROG_LOAD_R4 0x34
#define NAND_OP_PROG_EXEC    0x10
#define NAND_OP_ERASE        0xd8

/* Feature addresses and the bits inside them */
#define NAND_FEAT_LOCK       0xa0
#define NAND_FEAT_CFG        0xb0
#define NAND_FEAT_STATUS     0xc0

#define NAND_ST_OIP          0x01
#define NAND_ST_WEL          0x02
#define NAND_ST_EFAIL        0x04
#define NAND_ST_PFAIL        0x08
#define NAND_ST_ECC_MASK     0x30
#define NAND_ST_ECC_CLEAN    0x00
#define NAND_ST_ECC_FIXED    0x10
#define NAND_ST_ECC_UNCOR    0x20

#define NAND_CFG_QE          0x01
#define NAND_CFG_ECC_EN      0x10
#define NAND_CFG_OTP_EN      0x40

enum {
    NAND_WR_NONE = 0,
    NAND_WR_LOAD,                       /* program load: bytes into the cache */
    NAND_WR_SET_FEATURE,
};

struct IPQNand {
    uint8_t *data;              /* the die: NAND_STRIDE bytes per page */
    uint32_t bytes;             /* size of that array */
    uint32_t pages;             /* pages in the part (bytes / stride) */
    uint8_t id[2];              /* manufacturer, device */
    int id_len;

    uint8_t status;             /* 0xc0: OIP/WEL/E_FAIL/P_FAIL/ECC */
    uint8_t cfg;                /* 0xb0: QE, ECC enable, OTP */
    uint8_t lock;               /* 0xa0: block lock bits */
    int oip;                    /* busy for exactly one status read */
    int ecc;                    /* ECC bits reported by that same read */

    uint32_t page;              /* row latched by PAGE READ / PROG EXECUTE */
    uint32_t col;               /* column inside the page cache */
    uint8_t cache[NAND_STRIDE]; /* the die's internal page cache */
    int cache_valid;

    int wr_kind;                /* NAND_WR_*: what the data phase does */
    uint8_t wr_feature;         /* feature register a SET_FEATURE targets */

    IPQDieStore store;          /* nand-file= / nand-drive=, closed when RAM only */
};


/* Erased silicon is 0xff; the factory state of this part is "all erased". */
static void nand_fill_erased(uint8_t *p, uint32_t len)
{
    memset(p, 0xff, len);
}

/* ---- the NAND die store ---------------------------------------------- */

/*
 * The same contract as the NOR die: the store *is* the part.  A NAND is created
 * as an erased part on the first run (there is no NAND image in these dumps to
 * copy -- the board ships blank) and loaded instead of one afterwards, and
 * every program and erase is mirrored into it.  An erase is flushed and a page
 * program is not: the page cache survives QEMU exiting, and QEMU exiting is
 * what a power cut is here.
 */
static void nand_persist(const void *src, uint32_t off, uint32_t len)
{
    IPQNand *n = ipq_board->nand;

    if (!n || off >= n->bytes) {
        return;
    }
    ipq_die_store_pwrite(&n->store, src, MIN(len, n->bytes - off), off);
}

/* Returns true when the store already held a die (so it must not be erased). */
static bool nand_persist_open(IPQNand *n, const char *path, const char *drive,
                              const char *image)
{
    uint64_t len;

    /*
     * Unlike the NOR, the length here is not a choice: a NAND die carries the
     * spare area, so its length is pages * (page + spare).  A store of any
     * other length is a different part, not a smaller one.
     */
    len = ipq_die_store_bind(&n->store, path, drive, n->bytes, true);
    if (!len) {
        if (!ipq_die_store_writable(&n->store)) {
            error_report("ipq4019: nand store %s is empty and cannot be "
                         "written, so there is no die to boot from",
                         path ? path : drive);
            exit(1);
        }
        return false;
    }
    if (!ipq_die_store_pread(&n->store, n->data, n->bytes, 0)) {
        error_report("ipq4019: reading the nand store %s failed",
                     path ? path : drive);
        exit(1);
    }
    if (image && ipq_die_store_blank(n->data, n->bytes)) {
        fprintf(stderr, "ipq4019: nand store %s is blank (all 0x00): "
                "programming it from %s\n", path ? path : drive, image);
        return false;
    }
    return true;
}

static void nand_persist_snapshot(IPQNand *n)
{
    if (!ipq_die_store_open(&n->store)) {
        return;
    }
    ipq_die_store_pwrite(&n->store, n->data, n->bytes, 0);
    ipq_die_store_sync(&n->store);
}

/* ---- the NAND command set -------------------------------------------- */

static void nand_xact_reset(void)
{
    IPQNand *n = ipq_board->nand;

    if (!n) {
        return;
    }
    n->wr_kind = NAND_WR_NONE;
    n->col = 0;
}

static uint8_t nand_feature_read(uint8_t reg)
{
    IPQNand *n = ipq_board->nand;
    uint8_t v;

    switch (reg) {
    case NAND_FEAT_STATUS:
        v = n->status;
        if (n->oip) {
            v |= NAND_ST_OIP;
        } else {
            v |= n->ecc;
            n->ecc = NAND_ST_ECC_CLEAN;
        }
        /*
         * The die is fast enough to finish between commands, so it reports
         * busy for exactly one read: a driver that polls has to go round the
         * loop once, and one that does not is not poisoned by a bit that
         * never clears.  Same trade as the NOR's wip_pending.
         */
        n->oip = 0;
        n->status &= ~NAND_ST_OIP;
        break;
    case NAND_FEAT_CFG:
        v = n->cfg;
        break;
    case NAND_FEAT_LOCK:
        v = n->lock;
        break;
    default:
        v = 0x00;
        break;
    }
    return v;
}

static void nand_feature_write(uint8_t reg, uint8_t v)
{
    IPQNand *n = ipq_board->nand;

    switch (reg) {
    case NAND_FEAT_CFG:
        n->cfg = v & (NAND_CFG_QE | NAND_CFG_ECC_EN | NAND_CFG_OTP_EN);
        break;
    case NAND_FEAT_LOCK:
        n->lock = v;
        break;
    default:
        break;
    }
}

static void nand_page_read(uint32_t page)
{
    IPQNand *n = ipq_board->nand;

    n->page = page;
    if (page < n->pages) {
        memcpy(n->cache, n->data + page * NAND_STRIDE, NAND_STRIDE);
    } else {
        nand_fill_erased(n->cache, NAND_STRIDE);
    }
    n->cache_valid = 1;
    n->oip = 1;
    n->ecc = NAND_ST_ECC_CLEAN;         /* on-die ECC: nothing to report */
    if (env_true("QEMU_IPQ_NAND_LOG")) {
        qemu_log_mask(LOG_UNIMP, "nand: page read %u\n", page);
    }
}

static void nand_program_execute(uint32_t page)
{
    IPQNand *n = ipq_board->nand;
    uint32_t off;
    int need_erase = 0;

    n->status &= ~NAND_ST_WEL;
    n->oip = 1;
    if ((n->status & NAND_ST_EFAIL) || page >= n->pages ||
        (ipq_die_store_open(&n->store) &&
         !ipq_die_store_writable(&n->store))) {
        n->status = (n->status & ~NAND_ST_PFAIL) | NAND_ST_PFAIL;
        return;
    }
    off = page * NAND_STRIDE;
    /*
     * Programming can only clear bits.  Do it bit by bit so a page that was
     * written without an erase keeps the AND of both writes -- that is what
     * the silicon does, and it is how a broken FTL shows itself as garbage
     * rather than as a silently correct page.
     */
    for (uint32_t i = 0; i < NAND_STRIDE; i++) {
        uint8_t old = n->data[off + i];
        uint8_t want = n->cache[i];

        if ((want & ~old) != 0 && i < NAND_PAGE) {
            need_erase = 1;
        }
        n->data[off + i] = old & want;
    }
    nand_persist(n->data + off, off, NAND_STRIDE);
    if (need_erase && env_true("QEMU_IPQ_NAND_LOG")) {
        qemu_log_mask(LOG_UNIMP, "nand: page %u programmed over an erased-"
                      "only area; bits stayed 0\n", page);
    }
}

static void nand_block_erase(uint32_t row)
{
    IPQNand *n = ipq_board->nand;
    uint32_t blk = row / NAND_PPBLK;
    uint32_t off = blk * NAND_BLKBYTES;

    n->status &= ~NAND_ST_WEL;
    n->oip = 1;
    if ((n->status & NAND_ST_EFAIL) || off + NAND_BLKBYTES > n->bytes ||
        (ipq_die_store_open(&n->store) &&
         !ipq_die_store_writable(&n->store))) {
        n->status |= NAND_ST_EFAIL;
        return;
    }
    memset(n->data + off, 0xff, NAND_BLKBYTES);
    nand_persist(n->data + off, off, NAND_BLKBYTES);
    ipq_die_store_sync(&n->store);
    if (env_true("QEMU_IPQ_NAND_LOG")) {
        qemu_log_mask(LOG_UNIMP, "nand: erase block %u (row %u)\n", blk, row);
    }
}

/* Opcode phase for the NAND die.  Sets up addr/dummy counts in the QUP. */
static void nand_opcode(IPQQupl *q, uint8_t op)
{
    IPQNand *n = ipq_board->nand;

    q->nand_read = 0;
    n->wr_kind = NAND_WR_NONE;

    switch (op) {
    case NAND_OP_RDID:
        /*
         * One turnaround clock before the id: the die drives the manufacturer
         * only on the second clock after 9fh.
         *
         * The 5.6 kernel knows it even though it asks for no dummy clocks
         * (SPINAND_READID_OP(0, ...)): macronix_spinand_detect() skips the
         * first byte -- "Macronix SPI NAND read ID needs a dummy byte, so the
         * first byte in raw_id is garbage" -- matches id[1] against 0xc2 and
         * takes the device byte from id[2].  Get this clock wrong and a die
         * that answers immediately reads back as `unknown raw ID c2120000`;
         * get the part wrong and it reads back as `00c21400`.
         */
        q->dummy_left = 1;
        q->rx_buf[0] = n->id[0];
        q->rx_buf[1] = n->id[1];
        q->rx_len = n->id_len;
        q->rx_wrap = 1;             /* C212C212 on the board, for as long as
                                       the master keeps clocking */
        break;
    case NAND_OP_GET_FEATURE:
    case NAND_OP_SET_FEATURE:
        q->addr_left = 1;
        break;
    case NAND_OP_RESET:
        nand_xact_reset();
        nand_fill_erased(n->cache, NAND_STRIDE);
        n->cache_valid = 0;
        n->status = 0;
        n->oip = 0;
        break;
    case NAND_OP_WREN:
        n->status |= NAND_ST_WEL;
        break;
    case NAND_OP_WRDI:
        n->status &= ~NAND_ST_WEL;
        break;
    case NAND_OP_PAGE_READ:
    case NAND_OP_PROG_EXEC:
    case NAND_OP_ERASE:
        q->addr_left = 3;               /* 24-bit page / block address */
        break;
    case NAND_OP_READ_CACHE:
        /*
         * 03h is the plain READ FROM CACHE, and like its x1/x2/x4 siblings it
         * puts one dummy byte between the column address and the first data
         * byte (macronix.c: SPINAND_PAGE_READ_FROM_CACHE_OP(false, 0, 1, ...)).
         * The dummy is a turnaround, not a data clock: the die must not spend
         * a byte of the cache on it.  Without this the dummy consumed cache
         * column 0 and every later byte arrived one early -- invisible to the
         * 5.6 kernel, which reads through 0Bh, but it shifted every byte the
         * RouterBOOT cave read through 03h.
         */
        q->addr_left = 2;               /* 16-bit column */
        q->dummy_left = 1;
        break;
    case NAND_OP_READ_CACHE_F:
    case NAND_OP_READ_CACHE_2:
    case NAND_OP_READ_CACHE_4:
        q->addr_left = 2;
        q->dummy_left = 1;
        break;
    case NAND_OP_PROG_LOAD:
    case NAND_OP_PROG_LOAD_R:
    case NAND_OP_PROG_LOAD_4:
    case NAND_OP_PROG_LOAD_R4:
        q->addr_left = 2;               /* 16-bit column */
        n->wr_kind = NAND_WR_LOAD;
        break;
    default:
        break;
    }
    if (env_true("QEMU_IPQ_NAND_LOG")) {
        qemu_log_mask(LOG_UNIMP, "nand: op 0x%02x\n", op);
    }
}

/* The address phase finished: run whatever the command latched. */
static void nand_address_done(IPQQupl *q)
{
    IPQNand *n = ipq_board->nand;

    switch (q->opcode) {
    case NAND_OP_GET_FEATURE:
        q->rx_buf[0] = nand_feature_read((uint8_t)q->last_addr);
        q->rx_len = 1;
        break;
    case NAND_OP_SET_FEATURE:
        n->wr_feature = (uint8_t)q->last_addr;
        n->wr_kind = NAND_WR_SET_FEATURE;
        break;
    case NAND_OP_PAGE_READ:
        nand_page_read(q->last_addr & 0xffffff);
        break;
    case NAND_OP_PROG_EXEC:
        if (!(n->status & NAND_ST_WEL)) {
            if (env_true("QEMU_IPQ_NAND_LOG")) {
                qemu_log_mask(LOG_UNIMP, "nand: program without WEL ignored\n");
            }
            break;
        }
        nand_program_execute(q->last_addr & 0xffffff);
        break;
    case NAND_OP_ERASE:
        if (!(n->status & NAND_ST_WEL)) {
            if (env_true("QEMU_IPQ_NAND_LOG")) {
                qemu_log_mask(LOG_UNIMP, "nand: erase without WEL ignored\n");
            }
            break;
        }
        nand_block_erase(q->last_addr & 0xffffff);
        break;
    case NAND_OP_READ_CACHE:
    case NAND_OP_READ_CACHE_F:
    case NAND_OP_READ_CACHE_2:
    case NAND_OP_READ_CACHE_4:
        n->col = q->last_addr & 0xffff;
        q->nand_read = 1;
        break;
    case NAND_OP_PROG_LOAD:
    case NAND_OP_PROG_LOAD_R:
    case NAND_OP_PROG_LOAD_4:
    case NAND_OP_PROG_LOAD_R4:
        n->col = q->last_addr & 0xffff;
        break;
    default:
        break;
    }
}

/* One byte in the data phase, written by the master. */
static void nand_data_byte(uint8_t b)
{
    IPQNand *n = ipq_board->nand;

    switch (n->wr_kind) {
    case NAND_WR_LOAD:
        n->cache[n->col % NAND_STRIDE] = b;
        n->col++;
        break;
    case NAND_WR_SET_FEATURE:
        nand_feature_write(n->wr_feature, b);
        n->wr_kind = NAND_WR_NONE;
        break;
    default:
        break;
    }
}

/* One byte out of the data phase, read by the master. */
static uint8_t nand_miso(IPQQupl *q)
{
    IPQNand *n = ipq_board->nand;

    if (q->nand_read) {
        uint8_t v = n->cache_valid ? n->cache[n->col % NAND_STRIDE] : 0xff;

        n->col++;
        q->data_bytes++;
        die_dbg_note(v);
        return v;
    }
    if (q->rx_pos < q->rx_len) {
        return q->rx_buf[q->rx_pos++];
    }
    if (q->rx_wrap && q->rx_len > 0) {
        return q->rx_buf[q->rx_pos++ % q->rx_len];
    }
    return 0x00;
}

/* ---- instantiation ---------------------------------------------------- */

/*
 * QEMU_IPQ_NAND=1 puts the part on the board; QEMU_IPQ_NAND_PERSIST=... both
 * instantiates it and gives it a die file.  Geometry is fixed to the part the
 * vendor DTS names, because the whole point of the file is that it is a raw
 * dump of *that* chip: 1024 blocks of 64 pages, 2048+64 bytes each.
 */
/*
 * Whether each die was loaded from its store instead of programmed from an
 * image on this run.  The boot chain always comes out of the die, so these two
 * are what tells a run that booted what it was given from one that booted the
 * die it already had.
 */
static bool ipq_nor_store_loaded;
static bool ipq_nand_store_loaded;

/* ====================================================================== */
/* QPIC: the parallel NAND controller (and the BAM that feeds it)         */
/* ====================================================================== */

/*
 * The other way a board can carry a NAND: not a serial die behind the QUP's
 * second chip select, but a parallel one behind the QPIC controller, which
 * drives the bus itself and hands the bytes to a BAM.  The hAP ac3 is such a
 * board -- 16 MiB NOR for the boot chain, 128 MiB parallel NAND for the
 * installation -- and RouterBOOT reaches for the NAND early, so the model has
 * to answer the register protocol *and* run the engine, or the first poll
 * never finishes.
 *
 * Register offsets are the mainline ones (nand-qpic-common.h), the BAM is the
 * v1.7 layout the board's own DTS names (`qcom,bam-v1.7.0`: EE registers at
 * 0x3000, pipes at 0x13000 + 0x1000*n), and a command element carries the
 * NANDc register offset in its low 24 bits, relative to the QPIC block --
 * which is how the driver builds it (`bam_offset + reg_off`, 0x30000 here).
 *
 * The die itself is the same abstraction as the SPI NAND: a byte array in
 * which every page is NAND_PAGE data bytes followed by NAND_OOB spare bytes,
 * erased to 0xff, with a store behind it when the board's programming has to
 * survive the machine exiting.
 */
#define IPQ_QPIC_BAM_BASE    (IPQ_QPIC_BASE + 0x00004000u)  /* 0x07984000 */
#define IPQ_QPIC_BAM_SIZE    0x0001a000u
/*
 * The BAM node in the board tree maps 0x1a000 bytes, but the driver also uses
 * the pipe-attribute page at BAM+0x1c000 (QPIC+0x20100), reached through the
 * command pipe like any other register.  Those bytes are modelled as a plain
 * shadow: what is written there reads back.
 */
#define IPQ_QPIC_BAM_EXT     0x00006000u
#define IPQ_QPIC_NANDC_BASE  (IPQ_QPIC_BASE + 0x00030000u)  /* 0x079b0000 */
#define IPQ_QPIC_NANDC_SIZE  0x00001000u
/* what a command element puts in its low 24 bits (see qpic_bam_ce) */
#define QN_CE_BASE           ((IPQ_QPIC_BASE + 0x00030000u) & 0xffffffu)
#define QN_BAM_CE            ((IPQ_QPIC_BASE + 0x00004000u) & 0xffffffu)

/* NANDc register offsets (nand-qpic-common.h) */
#define QN_CMD              0x000
#define QN_ADDR0            0x004
#define QN_ADDR1            0x008
#define QN_CHIP_SEL         0x00c
#define QN_EXEC             0x010
#define QN_FLASH_STATUS     0x014
#define QN_BUFFER_STATUS    0x018
#define QN_DEV0_CFG0        0x020
#define QN_DEV0_CFG1        0x024
#define QN_DEV0_ECC_CFG     0x028
#define QN_READ_ID          0x040
#define QN_READ_STATUS      0x044
#define QN_DEV_CMD0         0x0a0
#define QN_DEV_CMD1         0x0a4
#define QN_DEV_CMD2         0x0a8
#define QN_DEV_CMD_VLD      0x0ac
#define QN_CW_DETECT_CFG    0x0e8
#define QN_CW_DETECT_STATUS 0x0ec
#define QN_EBI2_ECC_BUF_CFG 0x0f0
#define QN_BUF_ACC          0x100
#define QN_CTRL             0xf00
#define QN_VERSION          0xf08
#define QN_RD_LOC0          0xf20
#define QN_RD_LOC_LAST0     0xf40

/* NAND_FLASH_CMD opcodes */
#define QN_OP_PAGE_READ         0x2
#define QN_OP_PAGE_READ_ECC     0x3
#define QN_OP_PAGE_READ_SPARE   0x4
#define QN_OP_ONFI_READ         0x5
#define QN_OP_PROGRAM_PAGE      0x6
#define QN_OP_PROGRAM_PAGE_ECC  0x7
#define QN_OP_PROGRAM_SPARE     0x9
#define QN_OP_BLOCK_ERASE       0xa
#define QN_OP_FETCH_ID          0xb
#define QN_OP_CHECK_STATUS      0xc
#define QN_OP_RESET_DEVICE      0xd

#define QN_CMD_OP_MASK          0x0fu       /* bits 4/5 are PAGE_ACC/LAST_PAGE */
#define QN_CFG0_CW_PER_PAGE     0x1c0u      /* bits 8:6, steps - 1 */

/* BAM registers (v1.7 layout) */
#define QB_CTRL             0x00000
#define QB_DESC_TRSHLD      0x00008
#define QB_IRQ_STTS         0x00014
#define QB_IRQ_CLR          0x00018
#define QB_IRQ_EN           0x0001c
#define QB_CNFG_BITS        0x0007c
#define QB_REVISION         0x01000
#define QB_NUM_PIPES        0x01008
#define QB_IRQ_SRCS         0x03010
#define QB_IRQ_SRCS_MSK     0x03014
#define QB_IRQ_SRCS_EE      0x03000
#define QB_IRQ_SRCS_MSK_EE  0x03004
#define QB_PIPE_BASE        0x13000
#define QB_PIPE_STRIDE      0x1000
#define QB_P_CTRL           0x000
#define QB_P_RST            0x004
#define QB_P_HALT           0x008
#define QB_P_IRQ_STTS       0x010
#define QB_P_IRQ_CLR        0x014
#define QB_P_IRQ_EN         0x018
#define QB_P_SW_OFSTS       0x800
#define QB_P_EVNT_REG       0x818
#define QB_P_DESC_FIFO      0x81c
#define QB_P_FIFO_SIZES     0x820
#define QB_P_DATA_FIFO      0x824
#define QB_P_EVNT_TRSHLD    0x828
#define QB_P_EVNT_DEST      0x82c

#define QB_P_EN             (1u << 1)
#define QB_P_DIRECTION      (1u << 3)
#define QB_P_SYS_MODE       (1u << 5)
#define QB_P_CMD_MODE       (1u << 16)      /* command (CE) pipe */

#define QB_P_IRQ_PRCSD      (1u << 0)
#define QB_P_IRQ_OUT_DESC   (1u << 3)
#define QB_P_IRQ_ERR        (1u << 4)
#define QB_P_IRQ_TRNSFR_END (1u << 5)

#define QB_PIPES            3               /* tx, rx, cmd */
#define QB_PIPE_TX          0
#define QB_PIPE_RX          1
#define QB_PIPE_CMD         2

typedef struct IPQParNand {
    uint8_t *data;              /* NAND_STRIDE bytes per page */
    uint32_t bytes;
    uint32_t pages;
    uint8_t id[8];
    int id_len;
    IPQDieStore store;          /* qpic-nand-file= / -drive= */
} IPQParNand;

/*
 * The controller works a codeword at a time: one ECC step is 512 bytes of
 * page data, and a step takes QPIC_CW_SIZE bytes of the raw page once its
 * parity and spare bytes are counted in (2112 / 4 = 528 here).  The buffer at
 * FLASH_BUF_ACC holds exactly one such window: what a read location asks for
 * lands at its own offset inside the window, and a program consumes the same
 * window, so the two paths stay symmetric.
 */
#define QPIC_ECC_STEP   512u
#define QPIC_CW_STEPS   (NAND_PAGE / QPIC_ECC_STEP)
#define QPIC_CW_SIZE    (NAND_STRIDE / QPIC_CW_STEPS)
#define QPIC_IO_MAX     (NAND_STRIDE * 2u)  /* a page, plus one in flight */
#define QPIC_RX_DECS    32u                 /* queued rx descriptors */

struct IPQQpic {
    MemoryRegion mr;
    IPQParNand *die;

    uint32_t reg[IPQ_QPIC_NANDC_SIZE / 4];

    uint8_t buf[NAND_STRIDE];   /* one codeword window, at BUF_ACC offset 0 */
    bool loc_last;              /* the EXEC uses the LAST_CW read locations */

    uint8_t tx_stage[QPIC_IO_MAX];  /* program bytes the data pipe delivered */
    uint32_t tx_len;
    uint32_t txn_wr;            /* codeword the next program EXEC lands on */
    uint32_t tx_page;           /* page the staged bytes are consumed for */
    bool tx_page_set;
    bool tx_seq;                /* tx_page has seen a page-sequence window */
    uint32_t tx_used;           /* page-sequence bytes of tx_page programmed */
    bool tx_lost;               /* a staging overflow was reported */
    /*
     * A program EXEC whose payload was still in flight on the data pipe.
     * The driver rings the data descriptors and the command batch on two
     * different pipes and a doorbell only drains one of them, so the command
     * side can reach the EXEC before the bytes are staged.  Programming the
     * short page was silent corruption (the rest stayed queued and shifted
     * every later codeword), so the EXEC is held here and completed when the
     * data arrives.
     *
     * The entries belong to one page.  A window of a different page arriving
     * while one is still waiting means the staged bytes can no longer be
     * attributed to either page; the staging is dropped (loudly) rather than
     * shifted into the new page, because a shift corrupts every page after it.
     */
    struct {
        uint32_t page, at, cw, need;
        bool spare;
    } pgm_pend[8];
    unsigned pgm_n;

    uint8_t rd_stream[QPIC_IO_MAX]; /* bytes a read produced, not yet delivered */
    uint32_t rd_len;
    uint32_t rd_page;           /* page rd_stream is collecting */
    uint32_t txn_rd;            /* codeword the next read EXEC lands on */
    bool rd_lost;               /* a read overflow was reported */

    /*
     * A read puts its window into rd_stream and the BAM hands it to the rx
     * descriptors the data pipe queued; the two pipes can be rung in either
     * order (Linux queues the descriptors first, RouterBOOT runs the command
     * pipe first), so both ends just fill and drain the same FIFO.
     *
     * The page is read one codeword per EXEC, so the windows are appended to
     * the same stream and a reader that drains less than a whole window (the
     * booter takes the 64-byte EC header in 16-byte transfers) still gets the
     * page's bytes in order.  The stream is dropped when an EXEC addresses the
     * first codeword, or a different page: that is where a page read starts.
     */
    uint32_t rx_addr[QPIC_RX_DECS];
    uint32_t rx_len[QPIC_RX_DECS];
    uint32_t rx_done[QPIC_RX_DECS];
    uint32_t rx_n;

    uint32_t last_page;         /* page the window belongs to (logging) */
};

static bool ipq_qpic_store_loaded;

/*
 * Protocol violations always print.  Each one means the model misread the
 * controller's stream, and every one of them costs correctness, so they are
 * reported without needing an environment variable; they are capped so a
 * systematic mismatch cannot flood the console.
 */
void qpic_warn(const char *fmt, ...)
{
    static unsigned n;
    va_list ap;

    if (++n > 40) {
        if (n == 41) {
            fprintf(stderr, "qpic: anomaly: further reports suppressed\n");
        }
        return;
    }
    fprintf(stderr, "qpic: anomaly: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- the die ---------------------------------------------------------- */

static void qpic_die_persist(IPQParNand *d, const void *src, uint32_t off,
                             uint32_t len)
{
    if (off >= d->bytes) {
        return;
    }
    ipq_die_store_pwrite(&d->store, src, MIN(len, d->bytes - off), off);
}

static void qpic_die_erase(IPQParNand *d, uint32_t page)
{
    uint32_t blk = page / NAND_PPBLK;
    uint32_t off = blk * NAND_BLKBYTES;
    uint32_t len = NAND_BLKBYTES;

    /*
     * A block address past the part does not decode, so there is nothing to
     * erase -- and this must not walk off the end of the array when a
     * bootloader believes the part is larger than the die it was given.
     */
    if (blk >= d->pages / NAND_PPBLK) {
        return;
    }
    memset(d->data + off, 0xff, len);
    qpic_die_persist(d, d->data + off, off, len);
    ipq_die_store_sync(&d->store);
}

static void qpic_die_program(IPQParNand *d, uint32_t page, uint32_t col,
                             const uint8_t *buf, uint32_t len)
{
    uint32_t off = page * NAND_STRIDE;
    uint32_t i;

    if (page >= d->pages || col >= NAND_STRIDE) {
        return;
    }
    /*
     * Programming a NAND only clears bits, and 0xff is a no-op, so the page
     * keeps whatever was never written.  That is the same rule the SPI part
     * uses, and it is what makes a double program of a zeroed byte fail
     * exactly like the silicon does.
     *
     * The column matters: a page write lands at column 0 with all 2112 bytes,
     * a spare-only write lands at column 2048 with 64, and both are the same
     * "program" opcode with a different address.
     */
    for (i = 0; i < len && col + i < NAND_STRIDE; i++) {
        uint32_t at = col + i;

        /*
         * The bad-block marker is the one part of the spare area a codeword
         * program never touches: the QPIC's BAD BLOCK BYTE (0x1d1, the last
         * codeword's byte 465, which is spare byte 1) is excluded from ECC
         * programming, and for a 2048+64 part the marker lives at spare
         * bytes 0..1.  What a codeword program carries for that area is the
         * driver's ECC placeholder -- the part gets computed parity there
         * instead -- and storing those placeholders verbatim zeroed the
         * marker, so every programmed block read back as bad: yaffs could
         * not remount its own writes ("block %d is bad"), and RouterBOOT
         * could not read a boot partition that was written as yaffs.
         * Spare-only programs (the ones that do move the marker when a block
         * is retired) still program it.
         */
        if (col < NAND_PAGE && at >= NAND_PAGE && at <= NAND_PAGE + 1) {
            continue;
        }
        d->data[off + at] &= buf[i];
    }
    qpic_die_persist(d, d->data + off, off, NAND_STRIDE);
}

/* ---- the controller --------------------------------------------------- */

static void qpic_log_stream(IPQQpic *q, const char *what, uint32_t addr,
                            uint32_t off, const uint8_t *p, uint32_t len)
{
    if (!env_true("QEMU_IPQ_QPIC_LOG")) {
        return;
    }
    qpic_log("qpic: %s %08x +%u from page %u cw off %u: "
             "%02x %02x %02x %02x %02x %02x %02x %02x\n",
             what, addr, len, q->last_page, off,
             p[0], len > 1 ? p[1] : 0, len > 2 ? p[2] : 0,
             len > 3 ? p[3] : 0, len > 4 ? p[4] : 0,
             len > 5 ? p[5] : 0, len > 6 ? p[6] : 0,
             len > 7 ? p[7] : 0);
}

/* How many codewords the CFG0 register says the page is split into. */
static uint32_t qpic_cw_steps(IPQQpic *q)
{
    return ((q->reg[QN_DEV0_CFG0 / 4] & QN_CFG0_CW_PER_PAGE) >> 6) + 1;
}

/*
 * The stride between two codewords is the CFG0 "user data" size, not the
 * page's raw stride: with ECC on, MikroTik's driver frames a codeword as 516
 * bytes (500 data + 16 spare on the last step) and its chunks tile the flat
 * 2048-byte page exactly; with ECC off the frame is the whole 528.  Sizing
 * the frame from the page stride instead put every chunk 16 bytes further
 * than the driver sent it, which scrambled the page: yaffs' write verify
 * ("write required 321 attempts", "disk is too small") and RouterBOOT's read
 * of an installer-written kernel both failed on it.
 */
static uint32_t qpic_cw_size(IPQQpic *q)
{
    uint32_t ud = (q->reg[QN_DEV0_CFG0 / 4] >> 9) & 0x3ff;

    return ud ? ud : QPIC_CW_SIZE;
}

/*
 * One read EXEC fills the window for the codeword it addresses.
 *
 * The read-location registers are the whole description of that window: the
 * driver sets them before each EXEC and the controller presents, at
 * FLASH_BUF_ACC + offset, the bytes that live at that offset inside the
 * codeword.  Walking the locations in order also gives the byte stream the
 * data pipe will drain (data first, then the spare bytes), which is what the
 * queued rx descriptors expect.
 *
 * A codeword's place in the page is its column when the command carries one
 * (raw reads and the last-codeword helpers address it that way) and otherwise
 * the running step index, because the ECC path leaves the column at 0 and
 * lets the controller advance by itself.
 *
 * The column is an absolute byte offset into the raw page, not a step number:
 * the bad-block helpers read the last codeword with column 528 * (steps - 1)
 * while CFG0 still describes a single step, so deriving the codeword from the
 * step counter put the window at offset 0 and made every programmed block look
 * bad (its data byte landed where the BBM byte is looked for).
 */
static void qpic_read_window(IPQQpic *q, uint32_t page, uint32_t op,
                             uint32_t col, uint32_t cw)
{
    uint32_t base = col ? col : cw * qpic_cw_size(q);
    uint32_t regs = q->loc_last ? QN_RD_LOC_LAST0 : QN_RD_LOC0;
    const uint8_t *die = page < q->die->pages ?
                         q->die->data + (size_t)page * NAND_STRIDE : NULL;
    int i;

    /*
     * A read transaction is one page (the four codeword EXECs of a page
     * read) or one column-addressed window (the bad-block helpers address the
     * spare area that way and the pipe only ever sees those four bytes).
     * Anything still in the stream when a new transaction starts was never
     * collected by the data pipe, so a descriptor the guest queued will never
     * be filled; say so instead of silently prepending stale bytes.
     */
    if (cw == 0 || col != 0 || page != q->rd_page) {
        if (q->rd_len) {
            qpic_warn("read page %u starts with %u byte(s) uncollected\n",
                      page, q->rd_len);
        }
        q->rd_len = 0;
        q->rd_page = page;
        q->rd_lost = false;
    }

    for (i = 0; i < 4; i++) {
        uint32_t v = q->reg[regs / 4 + i];
        uint32_t off = v & 0x3ff;
        uint32_t len = (v >> 16) & 0x3ff;
        uint32_t k;

        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            qpic_log("qpic:   loc%d v=%08x off=%u len=%u base=%u rd=%u\n",
                     i, v, off, len, base, q->rd_len);
        }
        for (k = 0; k < len && off + k < NAND_STRIDE; k++) {
            uint32_t d = base + off + k;
            uint8_t b = (die && d < NAND_STRIDE) ? die[d] : 0xff;

            q->buf[off + k] = b;
            if (q->rd_len < QPIC_IO_MAX) {
                q->rd_stream[q->rd_len++] = b;
            } else if (!q->rd_lost) {
                q->rd_lost = true;
                qpic_warn("read page %u: window overflow at %u byte(s)\n",
                          page, q->rd_len);
            }
        }
        if (v & (1u << 31)) {
            break;              /* this location ends the window */
        }
    }
    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        uint32_t n = q->rd_len;
        qpic_log("qpic: win page=%u base=%u regs=%08x %08x %08x %08x die=%d "
                 "got=%02x %02x %02x %02x die8=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                 page, base, q->reg[regs / 4], q->reg[regs / 4 + 1],
                 q->reg[regs / 4 + 2], q->reg[regs / 4 + 3],
                 die != NULL,
                 n > 3 ? q->rd_stream[n - 4] : 0xff,
                 n > 2 ? q->rd_stream[n - 3] : 0xff,
                 n > 1 ? q->rd_stream[n - 2] : 0xff,
                 n > 0 ? q->rd_stream[n - 1] : 0xff,
                 die && base + 0 < NAND_STRIDE ? die[base + 0] : 0xff,
                 die && base + 1 < NAND_STRIDE ? die[base + 1] : 0xff,
                 die && base + 2 < NAND_STRIDE ? die[base + 2] : 0xff,
                 die && base + 3 < NAND_STRIDE ? die[base + 3] : 0xff,
                 die && base + 4 < NAND_STRIDE ? die[base + 4] : 0xff,
                 die && base + 5 < NAND_STRIDE ? die[base + 5] : 0xff,
                 die && base + 6 < NAND_STRIDE ? die[base + 6] : 0xff,
                 die && base + 7 < NAND_STRIDE ? die[base + 7] : 0xff);
        if (col == 1584 && die) {
            qpic_log("qpic: bbm page=%u byte=%02x bblk=%u\n",
                     page, die[NAND_PAGE], page / NAND_PPBLK);
        }
    }
}

/*
 * How many bytes one program EXEC takes out of the data pipe.
 *
 * Linux queues the whole page before the first EXEC (516, 516, 516, then the
 * last codeword's 500 bytes), while MikroTik's older driver fed one codeword
 * per EXEC.  The windows tile the 2048-byte page exactly, so the size comes
 * from that tiling and from the CFG0 step size the driver framed the buffer
 * with -- not from the physical codeword, which also counts the ECC parity
 * and spare bytes the driver never sends.
 *
 * The last step is where this matters: for an ECC page it carries the rest of
 * the page (2048 - 3 * 516 = 500).  Sizing it as data + spare + parity + cfg
 * bits (527 for the traced configuration) made the held EXEC wait for bytes
 * that never arrive, and the next page's bytes then completed it: the stream
 * shifted by 27 bytes per page, the last-window write ran past the page into
 * the spare area, and every later page was written at the wrong offset.
 */
static uint32_t qpic_program_size(IPQQpic *q, uint32_t cw)
{
    uint32_t ecc = q->reg[QN_DEV0_ECC_CFG / 4];
    uint32_t steps = qpic_cw_steps(q);
    uint32_t n = qpic_cw_size(q);

    if ((ecc & 1) || steps <= 1) {
        return n;               /* ECC off: the step is one raw window */
    }
    if (cw + 1 == steps && (steps - 1) * n < NAND_PAGE) {
        return NAND_PAGE - (steps - 1) * n;
    }
    return n;
}

/* Bytes one complete program transaction moves for the configured layout. */
static uint32_t qpic_program_page_bytes(IPQQpic *q)
{
    uint32_t steps = qpic_cw_steps(q);

    if ((q->reg[QN_DEV0_ECC_CFG / 4] & 1) || steps <= 1) {
        return qpic_cw_size(q);
    }
    return MIN((uint32_t)NAND_PAGE, steps * qpic_cw_size(q));
}

static void qpic_program_do(IPQQpic *q, uint32_t page, uint32_t at,
                            uint32_t cw, uint32_t n)
{
    if (n) {
        if (env_true("QEMU_IPQ_QPIC_LOG") ||
            (env_true("QEMU_IPQ_QPIC_PGM") && cw == 0)) {
            qpic_log("qpic: prog page=%u cw=%u at=%u n=%u tx=%02x %02x %02x %02x "
                     "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     page, cw, at, n,
                     q->tx_stage[0], q->tx_stage[1], q->tx_stage[2],
                     q->tx_stage[3], q->tx_stage[4], q->tx_stage[5],
                     q->tx_stage[6], q->tx_stage[7], q->tx_stage[8],
                     q->tx_stage[9], q->tx_stage[10], q->tx_stage[11],
                     q->tx_stage[12]);
        }
        qpic_die_program(q->die, page, at, q->tx_stage, n);
        q->tx_len -= n;
        memmove(q->tx_stage, q->tx_stage + n, q->tx_len);
        ipq_die_store_sync(&q->die->store);
    }
}

/* Complete held EXECs, in the order they were issued, as data arrives. */
static void qpic_program_pending(IPQQpic *q)
{
    while (q->pgm_n && q->tx_len >= q->pgm_pend[0].need) {
        uint32_t page = q->pgm_pend[0].page;
        uint32_t at = q->pgm_pend[0].at;
        uint32_t cw = q->pgm_pend[0].cw;
        uint32_t need = q->pgm_pend[0].need;
        bool spare = q->pgm_pend[0].spare;

        qpic_program_do(q, page, at, cw, need);
        if (!spare) {
            q->tx_used += need;
        }
        memmove(&q->pgm_pend[0], &q->pgm_pend[1],
                (q->pgm_n - 1) * sizeof(q->pgm_pend[0]));
        q->pgm_n--;
    }
}

static void qpic_exec(IPQQpic *q)
{
    uint32_t cmd = q->reg[QN_CMD / 4];
    uint32_t op = cmd & QN_CMD_OP_MASK;
    uint32_t page = ((q->reg[QN_ADDR1 / 4] & 0xff) << 16) |
                    ((q->reg[QN_ADDR0 / 4] >> 16) & 0xffff);
    uint32_t col = q->reg[QN_ADDR0 / 4] & 0xffff;
    uint32_t steps = qpic_cw_steps(q);
    /*
     * The erase command is the odd one out: it carries the row in ADDR0[15:0]
     * (ADDR1[7:0] for the high bits) instead of ADDR0[31:16].  Reading it like
     * a page command made every erase land on page 0, so the blocks being
     * formatted were never erased and the filesystem could not be mounted.
     */
    uint32_t row_erase = col | ((q->reg[QN_ADDR1 / 4] & 0xff) << 16);

    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        qpic_log(
                      "qpic: exec op=%u page=%u col=%u cmd=%08x addr0=%08x "
                      "addr1=%08x cfg0=%08x ecc=%08x loc=%08x in=%u\n",
                      op, page, col, cmd,
                      q->reg[QN_ADDR0 / 4], q->reg[QN_ADDR1 / 4],
                      q->reg[QN_DEV0_CFG0 / 4], q->reg[QN_DEV0_ECC_CFG / 4],
                      q->reg[QN_RD_LOC0 / 4], q->tx_len);
    }

    switch (op) {
    case QN_OP_RESET_DEVICE:
        q->txn_rd = q->txn_wr = 0;
        break;

    case QN_OP_FETCH_ID:
        q->reg[QN_READ_ID / 4] =
            (uint32_t)q->die->id[0] |
            ((uint32_t)q->die->id[1] << 8) |
            ((uint32_t)q->die->id[2] << 16) |
            ((uint32_t)q->die->id[3] << 24);
        break;

    case QN_OP_CHECK_STATUS:
        q->reg[QN_READ_STATUS / 4] = 0x00;      /* ready, no failure */
        break;

    case QN_OP_BLOCK_ERASE:
        qpic_die_erase(q->die, row_erase);
        q->txn_rd = q->txn_wr = 0;
        break;

    case QN_OP_PAGE_READ:
    case QN_OP_PAGE_READ_ECC:
    case QN_OP_PAGE_READ_SPARE:
    case QN_OP_ONFI_READ: {
        uint32_t cw;

        if (col) {
            cw = MIN(col / QPIC_CW_SIZE, steps - 1);
        } else {
            if (q->txn_rd >= steps) {
                q->txn_rd = 0;
            }
            cw = q->txn_rd;
        }
        q->last_page = page;
        qpic_read_window(q, page, op, col, cw);
        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            qpic_log("qpic: rd window page=%u cw=%u col=%u len=%u loc=%08x\n",
                     page, cw, col, q->rd_len, q->reg[QN_RD_LOC0 / 4]);
        }
        q->txn_rd = cw + 1;
        break;
    }

    case QN_OP_PROGRAM_PAGE:
    case QN_OP_PROGRAM_PAGE_ECC:
    case QN_OP_PROGRAM_SPARE: {
        uint32_t cw = col ? MIN(col / QPIC_CW_SIZE, steps - 1) :
                            MIN(q->txn_wr, steps - 1);
        /* Same rule as the read side: a column is an absolute byte offset. */
        uint32_t at = col ? col : cw * qpic_cw_size(q);
        uint32_t need = qpic_program_size(q, cw);
        /*
         * A column-addressed program is a window of its own (spare bytes, the
         * bad-block marker): it is not a step of the four-codeword page
         * sequence, so it carries exactly what the pipe delivered for it,
         * bounded by the rest of the row.
         */
        bool spare = col != 0;
        uint32_t n;

        if (spare && at < NAND_STRIDE) {
            need = MIN(need, NAND_STRIDE - at);
        }

        /*
         * The staged bytes belong to one page.  A window of a different page
         * arriving while one is still waiting for its payload means the two
         * can no longer be told apart (the driver has moved on), so the
         * staging is dropped instead of shifted into the new page.
         */
        if (q->pgm_n && q->tx_page_set && page != q->tx_page) {
            qpic_warn("program page %u supersedes page %u with %u window(s) "
                      "still waiting (%u staged, %u used); dropping them\n",
                      page, q->tx_page, q->pgm_n, q->tx_len, q->tx_used);
            q->pgm_n = 0;
            q->tx_len = 0;
        }
        if (!q->tx_page_set || page != q->tx_page) {
            /*
             * A page whose windows did not add up to one transaction is the
             * other way the stream loses its alignment; the next page would
             * then be written shifted.
             */
            if (q->tx_page_set && q->tx_seq &&
                q->tx_used != qpic_program_page_bytes(q)) {
                qpic_warn("program page %u moved %u byte(s), expected %u\n",
                          q->tx_page, q->tx_used,
                          qpic_program_page_bytes(q));
            }
            q->tx_page = page;
            q->tx_page_set = true;
            q->tx_used = 0;
            q->tx_seq = false;
        }
        q->tx_lost = false;
        if (!spare) {
            q->tx_seq = true;
        }

        /*
         * Programming a NAND only clears bits, and the page keeps whatever
         * was never written, so the die ANDs the step in place.  The bytes
         * land where the read path will look for them: at the codeword's own
         * window, which is also the byte order the driver queued them in.
         *
         * A codeword is programmed only once its whole payload is staged:
         * consuming a short page would leave the remainder queued and shift
         * every later codeword into the wrong place.
         */
        n = spare ? MIN(q->tx_len, need) : need;
        if (n && q->tx_len >= n) {
            qpic_program_do(q, page, at, cw, n);
            if (!spare) {
                q->tx_used += n;
            }
        } else if (q->pgm_n < ARRAY_SIZE(q->pgm_pend)) {
            q->pgm_pend[q->pgm_n].page = page;
            q->pgm_pend[q->pgm_n].at = at;
            q->pgm_pend[q->pgm_n].cw = cw;
            q->pgm_pend[q->pgm_n].need = n ? n : need;
            q->pgm_pend[q->pgm_n].spare = spare;
            q->pgm_n++;
            if (env_true("QEMU_IPQ_QPIC_LOG")) {
                qpic_log("qpic: prog wait page=%u cw=%u need=%u staged=%u\n",
                         page, cw, need, q->tx_len);
            }
        } else {
            qpic_warn("program page %u cw=%u need=%u: wait queue full, "
                      "window dropped\n", page, cw, need);
        }
        q->last_page = page;
        q->txn_wr = cw + 1;
        break;
    }

    default:
        break;
    }
}

static uint32_t qpic_reg_read(IPQQpic *q, uint32_t off)
{
    if (off >= QN_BUF_ACC && off < QN_BUF_ACC + NAND_STRIDE) {
        return q->buf[off - QN_BUF_ACC];        /* byte addressable window */
    }
    if (off == QN_FLASH_STATUS) {
        /*
         * FS_READY_BSY_N is "ready" when set: the driver writes it back to
         * clear the status and then polls until it reappears, so a device
         * that is never busy returns it every time.
         *
         * Bit 23 is the WP# pin, and it is active low: MikroTik's erase path
         * caches this word and, when the bit is clear, refuses the erase with
         * "msm_nand_erase: Device is write protected" before touching the
         * part.  The die here is writable, so the pin is deasserted and the
         * bit reads back set.  (Bits 4, 8 and 16 are the DEV/OP/MPU error
         * flags the same code checks, and they stay clear.)
         */
        return 0x20u | (1u << 23);
    }
    if (off == QN_BUFFER_STATUS) {
        return 0;                               /* no ECC errors */
    }
    if (off == QN_VERSION) {
        /*
         * NAND_VERSION, mainline's field layout: major in bits 31:28, minor in
         * 27:16 (nand-qpic-common.h).  A v2 controller is 0x20000000; the old
         * 0x00020000 put "2" in the minor field, so MikroTik's
         * msm_nand_version_check read major 0.0, took its fallback path, and
         * never got an id out of the part (flashid=eeeeeeee).
         */
        return 0x20000000u;                     /* QPIC v2.0 */
    }
    if (off == QN_CTRL) {
        return q->reg[QN_CTRL / 4];
    }
    return q->reg[off / 4];
}

static void qpic_reg_write(IPQQpic *q, uint32_t off, uint32_t val)
{
    if (off >= QN_BUF_ACC && off < QN_BUF_ACC + NAND_STRIDE) {
        q->buf[off - QN_BUF_ACC] = (uint8_t)val;
        return;
    }
    q->reg[off / 4] = val;

    /*
     * A fresh address starts a fresh transaction: the ECC path writes the
     * page address once and then steps through the codewords with the column
     * left at zero, so the step counter is what says which codeword an EXEC
     * means.  Raw reads and the last-codeword helpers put the codeword in the
     * column instead, which qpic_exec prefers when it is set.
     */
    if (off == QN_ADDR0) {
        q->txn_rd = 0;
        q->txn_wr = 0;
    }
    /*
     * The read locations come in two banks and the one written last is the
     * one this EXEC uses: the first three steps use READ_LOCATION_0..3 and the
     * last step uses READ_LOCATION_LAST_CW_0..3 (qpic v2), whichever step it
     * is.  Tracking the bank keeps the model from having to guess from the
     * column, which the ECC path never sets.
     */
    if (off >= QN_RD_LOC0 && off < QN_RD_LOC0 + 16) {
        q->loc_last = false;
    }
    if (off >= QN_RD_LOC_LAST0 && off < QN_RD_LOC_LAST0 + 16) {
        q->loc_last = true;
    }

    if (off == QN_EXEC && (val & 1)) {
        qpic_exec(q);
    }
}

/*
 * Direct register accesses, separate from the command elements: a driver's
 * probe reads the version and writes the config registers before it posts a
 * single command, so "no exec at all" cannot tell a probe that failed early
 * from a driver that never bound.  These lines are few -- the bulk of the
 * traffic goes through the BAM -- so they stay readable during a scan.
 */
static uint64_t qpic_mmio_read(void *opaque, hwaddr off, unsigned size)
{
    IPQQpic *q = opaque;
    uint64_t v;

    if (off >= IPQ_QPIC_NANDC_SIZE) {
        return 0;
    }
    v = qpic_reg_read(q, (uint32_t)off);
    qpic_log("qpic: mmio read off=%#x -> %08x\n", (uint32_t)off, (uint32_t)v);
    return v;
}

static void qpic_mmio_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    IPQQpic *q = opaque;

    if (off >= IPQ_QPIC_NANDC_SIZE) {
        return;
    }
    qpic_log("qpic: mmio write off=%#x <- %08x\n", (uint32_t)off,
             (uint32_t)val);
    qpic_reg_write(q, (uint32_t)off, (uint32_t)val);
}

static const MemoryRegionOps qpic_ops = {
    .read = qpic_mmio_read,
    .write = qpic_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- the BAM ---------------------------------------------------------- */

typedef struct IPQQpicBamPipe {
    uint32_t ctrl, irq_stts, irq_en;
    uint32_t sw_ofsts, evnt_reg, desc_fifo, fifo_sizes;
    uint32_t data_fifo, evnt_trshld, evnt_dest;
    uint32_t cons;              /* next FIFO slot this model will read */
} IPQQpicBamPipe;

typedef struct IPQQpicBam {
    MemoryRegion mr;
    qemu_irq irq;               /* SPI 101, level high */
    uint32_t ctrl, desc_trshld, cfg_bits;
    uint32_t irq_stts, irq_en;
    uint32_t irq_srcs_msk, irq_srcs_msk_ee;
    uint32_t irq_observed;      /* sources the CPU last saw in BAM_IRQ_SRCS */
    unsigned irq_line;          /* this model's current output level */
    IPQQpicBamPipe pipe[QB_PIPES];
    uint32_t ext[IPQ_QPIC_BAM_EXT / 4];    /* pipe-attribute page and up */
} IPQQpicBam;

static IPQQpicBam *ipq_qpic_bam;

/* The pipes that have something to report, before any mask is applied. */
static uint32_t qpic_bam_irq_srcs(IPQQpicBam *b)
{
    uint32_t srcs = 0;
    int i;

    for (i = 0; i < QB_PIPES; i++) {
        if (b->pipe[i].irq_stts & b->pipe[i].irq_en) {
            srcs |= 1u << i;
        }
    }
    return srcs;
}

static uint32_t qpic_bam_irq_pending(IPQQpicBam *b)
{
    return qpic_bam_irq_srcs(b) & (b->irq_srcs_msk_ee | b->irq_srcs_msk);
}

/*
 * Raise the BAM's line the way the BLSP1 BAM does.  RouterBOOT polls the
 * command pipe, which is why the QPIC worked with no line at all, but a Linux
 * NAND driver enables the pipe's "transfer end" source and waits for the
 * interrupt -- with nothing driving SPI 101 the wait times out and the probe
 * reports "NAND is not detected".
 */
static void qpic_bam_update_irq(IPQQpicBam *b)
{
    uint32_t pending = qpic_bam_irq_pending(b);
    uint32_t fresh;

    if (!b->irq || !env_u32("QEMU_IPQ_QPIC_IRQ", 1)) {
        return;
    }
    if (!pending) {
        /*
         * Every source acknowledged: re-arm the edge for the next transfer.
         */
        b->irq_observed = 0;
        if (b->irq_line) {
            b->irq_line = 0;
            qemu_set_irq(b->irq, 0);
        }
        return;
    }
    /*
     * The line asserts when a pipe reports an enabled source the CPU has not
     * observed in BAM_IRQ_SRCS_EE yet, and drops when the CPU reads that
     * register -- an edge, not a held level.  A held level cannot cross the
     * RouterBOOT handover: its final read-ahead command batches complete
     * after its last wait_pipe(), so the command pipe keeps PRCSD/TRNSFR_END
     * latched with nobody left to acknowledge it (the booter waits on the
     * data pipe and jumps), and the kernel's SPS handler exits without
     * clearing sources of pipes it does not own.  A held level would re-fire
     * the SPI forever from that leftover -- the soft lockup the moment the
     * kernel enables the IRQ -- while an edge dies after one IRQ.  Only the
     * edge semantics are consistent with the same booter and kernel binaries
     * running on the silicon, where RouterOS boots from NAND.
     */
    fresh = pending & ~b->irq_observed;
    if (fresh && !b->irq_line) {
        b->irq_line = 1;
        qemu_set_irq(b->irq, 1);
    }
}

static unsigned qpgm_logs;

static void qpic_bam_data_in(IPQQpic *q, uint32_t addr, uint32_t len)
{
    uint8_t chunk[512];
    uint32_t done = 0;

    while (done < len) {
        uint32_t take = MIN(len - done, (uint32_t)sizeof(chunk));

        address_space_read(&address_space_memory, addr + done,
                           MEMTXATTRS_UNSPECIFIED, chunk, take);
        if (env_true("QEMU_IPQ_QPIC_PGM") && qpgm_logs < 400) {
            qpgm_logs++;
            qpic_log("qpic: datain addr=%08x len=%u stage=%u tx=%02x %02x "
                     "%02x %02x %02x %02x %02x %02x\n",
                     addr + done, take, q->tx_len,
                     chunk[0], chunk[1], chunk[2], chunk[3],
                     chunk[4], chunk[5], chunk[6], chunk[7]);
        }
        qpic_log_stream(q, "in", addr + done, q->tx_len, chunk, take);
        /*
         * The bytes stay staged until the command pipe reaches the program
         * EXEC they belong to, because the whole page arrives before the
         * first codeword is programmed.
         */
        if (q->tx_len + take <= QPIC_IO_MAX) {
            memcpy(q->tx_stage + q->tx_len, chunk, take);
            q->tx_len += take;
        } else if (!q->tx_lost) {
            q->tx_lost = true;
            qpic_warn("program staging overflow: %u byte(s) dropped\n", take);
        }
        done += take;
    }
    qpic_program_pending(q);
}

/*
 * Drain the descriptors the data pipe queued for a read.
 *
 * They are served from rd_stream, which the command pipe filled one codeword
 * at a time, so the guest sees the die's contents and not whatever the
 * controller's window happened to hold when the data pipe was rung.
 *
 * A descriptor whose length runs past the bytes produced so far simply waits:
 * the transfer is completed by the next read (or by this same pass, when the
 * command pipe ran after the data pipe).
 */
static void qpic_bam_data_out(IPQQpic *q)
{
    uint32_t pos = 0;

    while (q->rx_n && pos < q->rd_len) {
        uint32_t addr = q->rx_addr[0];
        uint32_t len = q->rx_len[0];
        uint32_t done = q->rx_done[0];
        uint8_t chunk[512];
        uint32_t take = MIN(MIN(len - done, (uint32_t)sizeof(chunk)),
                            q->rd_len - pos);
        uint32_t k;

        for (k = 0; k < take; k++) {
            chunk[k] = q->rd_stream[pos + k];
        }
        qpic_log_stream(q, "out", addr + done, pos, chunk, take);
        address_space_write(&address_space_memory, addr + done,
                            MEMTXATTRS_UNSPECIFIED, chunk, take);
        pos += take;
        q->rx_done[0] = done + take;
        if (q->rx_done[0] == len) {
            memmove(&q->rx_addr[0], &q->rx_addr[1],
                    (q->rx_n - 1) * sizeof(q->rx_addr[0]));
            memmove(&q->rx_len[0], &q->rx_len[1],
                    (q->rx_n - 1) * sizeof(q->rx_len[0]));
            memmove(&q->rx_done[0], &q->rx_done[1],
                    (q->rx_n - 1) * sizeof(q->rx_done[0]));
            q->rx_n--;
        }
    }
    if (pos) {
        memmove(q->rd_stream, q->rd_stream + pos, q->rd_len - pos);
        q->rd_len -= pos;
    }
}

/*
 * One command element: {cmd_and_addr, data, mask, reserved}, 16 bytes.  Bits
 * 31:24 of the first word are the command (0 = write, 1 = read) and bits 23:0
 * are the offset inside the QPIC block, so a CE write to 0x30000 lands on
 * NAND_FLASH_CMD, exactly like a CPU store would.  The command pipe's own FIFO
 * holds ordinary {addr,len} descriptors whose payload is an array of these.
 */
static uint64_t qpic_bam_read_raw(void *opaque, hwaddr off, unsigned size);
static void qpic_bam_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size);

static void qpic_bam_ce(IPQQpicBam *b, uint32_t ce_addr, uint32_t ce_size)
{
    IPQQpic *q = ipq_board->qpic;
    uint8_t raw[256];
    uint32_t i;

    if (!q || ce_size < 16) {
        return;
    }
    if (ce_size > sizeof(raw)) {
        ce_size = sizeof(raw);
    }
    address_space_read(&address_space_memory, ce_addr, MEMTXATTRS_UNSPECIFIED,
                       raw, ce_size);
    for (i = 0; i + 16 <= ce_size; i += 16) {
        uint32_t ca = (uint32_t)raw[i] | ((uint32_t)raw[i + 1] << 8) |
                      ((uint32_t)raw[i + 2] << 16) |
                      ((uint32_t)raw[i + 3] << 24);
        uint32_t data = (uint32_t)raw[i + 4] | ((uint32_t)raw[i + 5] << 8) |
                        ((uint32_t)raw[i + 6] << 16) |
                        ((uint32_t)raw[i + 7] << 24);
        uint32_t hi = (uint32_t)raw[i + 8] | ((uint32_t)raw[i + 9] << 8) |
                      ((uint32_t)raw[i + 10] << 16) |
                      ((uint32_t)raw[i + 11] << 24);
        uint32_t c = ca >> 24;
        uint32_t off = ca & 0xffffffu;

        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            qpic_log("qpic: ce %s off=%#x data=%08x\n",
                          c == 1 ? "read" : "write", off, data);
        }
        /*
         * A CE carries the *low 24 bits* of the absolute register address,
         * i.e. (QPIC base + BAM offset + register offset) & 0xffffff.  On this
         * SoC that is 0x9b0000 + reg_off.
         */
        if (off >= QN_BAM_CE && off - QN_BAM_CE < IPQ_QPIC_BAM_SIZE +
                                                    IPQ_QPIC_BAM_EXT) {
            uint32_t roff = off - QN_BAM_CE;

            /*
             * The command pipe's 24-bit address covers the whole QPIC block,
             * so a driver can read and write BAM registers with a command
             * element -- MikroTik's does, for the pipe-attribute page at
             * BAM+0x1c000 (QPIC+0x20100), which is where the SPS layer looks
             * before it hands a pipe to the NAND engine.
             */
            if (c == 1) {
                uint64_t dst = (uint64_t)hi << 32 | data;
                uint32_t v = qpic_bam_read_raw(b, roff, 4);

                qpic_log("qpic: ce read off=%#x -> %08x\n", off, v);
                address_space_write(&address_space_memory, dst,
                                    MEMTXATTRS_UNSPECIFIED, &v, 4);
            } else {
                qpic_log("qpic: ce write off=%#x <- %08x (bam)\n", off, data);
                qpic_bam_write(b, roff, data, 4);
            }
            continue;
        }
        if (off < QN_CE_BASE || off - QN_CE_BASE + 4 > IPQ_QPIC_NANDC_SIZE) {
            continue;       /* not a NANDc register: the BAM is not ours */
        }
        off -= QN_CE_BASE;
        if (c == 1) {
            /*
             * Register -> memory.  The destination is `data` plus the *upper
             * four bits* of the mask field (qcom_bam_dma.h, BAM v1.6+); taking
             * the mask as a full 32-bit high word turns a destination of
             * 0x8eaf408c into 0xffffffff8eaf408c when the driver fills the
             * mask like a write command, and the store then goes nowhere --
             * which is how an id read came back as the driver's 0xEE poison.
             */
            /*
             * The destination is the 32-bit `data` field.  The mask field does
             * not carry address bits here: MikroTik's driver fills it with
             * 0xffffffff on read commands (a copy of its write mask) while the
             * address it wants is a plain RAM pointer, so honouring the top
             * nibble would send every register read into no-man's land.
             */
            uint64_t dst = (uint64_t)data;
            uint32_t v = qpic_reg_read(q, off);

            qpic_log("qpic: ce read off=%#x dst=%08x%08x mask=%08x -> %08x\n",
                     off + QN_CE_BASE, (uint32_t)(dst >> 32), data, hi, v);
            address_space_write(&address_space_memory, dst,
                                MEMTXATTRS_UNSPECIFIED, &v, 4);
        } else {
            qpic_reg_write(q, off, data);
        }
    }
}

static void qpic_bam_run(IPQQpicBam *b, int n, uint32_t doorbell)
{
    IPQQpicBamPipe *p = &b->pipe[n];
    IPQQpic *q = ipq_board->qpic;
    uint32_t slots = p->fifo_sizes ? (p->fifo_sizes & 0xffff) / 8 : 0;
    uint32_t guard = 0;

    if (!q || !p->desc_fifo || !slots || !(p->ctrl & QB_P_EN)) {
        return;
    }
    {
        uint32_t tail = (doorbell / 8) % slots;

        if (p->cons >= slots) {
            p->cons = 0;
        }
        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            uint32_t i = p->cons, k;

            qpic_log("qpic: ring pipe=%d cons=%u tail=%u\n",
                     n, p->cons, tail);
            for (k = 0; k < 8 && i != tail; k++, i = (i + 1) % slots) {
                uint32_t e[2];

                address_space_read(&address_space_memory,
                                   p->desc_fifo + i * 8,
                                   MEMTXATTRS_UNSPECIFIED, e, sizeof(e));
                qpic_log("qpic:   slot %u: %08x %08x\n", i, e[0], e[1]);
            }
        }
        /*
         * The window produced by the command pipe stays in rd_stream until a
         * queued rx descriptor collects it: the pipes can be rung in either
         * order, and a transfer may run across several EXECs.
         */
        /*
         * The two pipes can be rung in either order.  Linux queues the rx
         * descriptors first and lets the command pipe run the read; RouterBOOT
         * runs the command pipe first and only then hands over the rx
         * descriptors.  Both are served here: whichever half arrives second
         * completes the transfer.
         */
        while (p->cons != tail && guard++ < 4096) {
            uint32_t d[2];

            address_space_read(&address_space_memory,
                               p->desc_fifo + p->cons * 8,
                               MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
            if (env_true("QEMU_IPQ_QPIC_LOG")) {
                qpic_log("qpic: desc pipe=%d cons=%u tail=%u db=%#x "
                         "addr=%08x w1=%08x\n",
                         n, p->cons, tail, doorbell, d[0], d[1]);
            }
            if (d[1] & 0xffff) {
                if (n == QB_PIPE_CMD) {
                    qpic_bam_ce(b, d[0], d[1] & 0xffff);
                } else if (p->ctrl & QB_P_DIRECTION) {
                    if (env_true("QEMU_IPQ_QPIC_LOG")) {
                        qpic_log(
                                      "qpic: rx desc %08x len %u\n",
                                      d[0], d[1] & 0xffff);
                    }
                    if (q->rx_n < ARRAY_SIZE(q->rx_addr)) {
                        q->rx_addr[q->rx_n] = d[0];
                        q->rx_len[q->rx_n] = d[1] & 0xffff;
                        q->rx_done[q->rx_n] = 0;
                        q->rx_n++;
                    } else {
                        qpic_warn("rx descriptor ring full: %08x len %u "
                                  "dropped\n", d[0], d[1] & 0xffff);
                    }
                } else {
                    if (env_true("QEMU_IPQ_QPIC_LOG")) {
                        qpic_log(
                                      "qpic: tx desc %08x len %u\n",
                                      d[0], d[1] & 0xffff);
                    }
                    qpic_bam_data_in(q, d[0], d[1] & 0xffff);
                }
            }
            p->cons = (p->cons + 1) % slots;
        }
        /*
         * A command pass may have produced a window while descriptors were
         * already queued, and a data pass may have queued descriptors for a
         * window that was produced earlier.  Both end here.
         */
        qpic_bam_data_out(q);
    }
    /*
     * A doorbell only completes something if this pass retired a descriptor.
     * The driver's last ring of a transfer can be a bare flush -- RouterBOOT
     * times out on a part with no kernel and rings once more, and the
     * installer's flash tool ends its command list the same way.  Latching
     * PRCSD for those left a source nobody acknowledges, and the next kernel
     * waited for BAM_IRQ_SRCS_EE to reach zero forever (soft lockup inside
     * msm_nand_bam_init); skipping the latch made RouterBOOT's own wait loop
     * miss completions and hang the other way.
     */
    if (guard) {
        p->irq_stts |= QB_P_IRQ_PRCSD | QB_P_IRQ_TRNSFR_END;
        p->irq_stts &= ~QB_P_IRQ_OUT_DESC;
        qpic_bam_update_irq(b);
    }
}

static uint64_t qpic_bam_read_raw(void *opaque, hwaddr off, unsigned size)
{
    IPQQpicBam *b = opaque;
    int n = -1;
    uint32_t roff = 0;

    if (off >= QB_PIPE_BASE) {
        uint32_t rel = off - QB_PIPE_BASE;

        n = rel / QB_PIPE_STRIDE;
        roff = rel % QB_PIPE_STRIDE;
        if (n >= QB_PIPES) {
            return 0;
        }
    }
    if (n >= 0) {
        IPQQpicBamPipe *p = &b->pipe[n];

        switch (roff) {
        case QB_P_CTRL:      return p->ctrl;
        case QB_P_IRQ_STTS:  return p->irq_stts;
        case QB_P_IRQ_EN:    return p->irq_en;
        case QB_P_SW_OFSTS:  return p->cons * 8;
        case QB_P_EVNT_REG:  return p->evnt_reg;
        case QB_P_DESC_FIFO: return p->desc_fifo;
        case QB_P_FIFO_SIZES:return p->fifo_sizes;
        default:             return 0;
        }
    }
    if (off >= IPQ_QPIC_BAM_SIZE) {
        unsigned idx = (off - IPQ_QPIC_BAM_SIZE) / 4;

        return idx < ARRAY_SIZE(b->ext) ? b->ext[idx] : 0;
    }
    if (off == QB_IRQ_SRCS_EE && env_true("QEMU_IPQ_BAM_DBG")) {
        static unsigned calls;
        calls++;
        if (calls < 12 || (calls % 500000) == 0) {
            int i;
            fprintf(stderr, "bam: srcs=%08x", qpic_bam_irq_srcs(b));
            for (i = 0; i < QB_PIPES; i++) {
                fprintf(stderr, " p%d{ctrl=%08x sts=%08x en=%08x cons=%u evnt=%08x}",
                        i, b->pipe[i].ctrl, b->pipe[i].irq_stts,
                        b->pipe[i].irq_en, b->pipe[i].cons,
                        b->pipe[i].evnt_reg);
            }
            fprintf(stderr, "\n");
        }
    }
    if (off == QB_IRQ_SRCS_EE || off == QB_IRQ_SRCS) {
        /*
         * The CPU observing the sources is what retires the line's edge (see
         * qpic_bam_update_irq): the sources stay latched, but the SPI drops
         * so a leftover no driver owns cannot re-fire it.
         */
        b->irq_observed = qpic_bam_irq_pending(b);
        if (b->irq_line) {
            b->irq_line = 0;
            if (env_u32("QEMU_IPQ_QPIC_IRQ", 1)) {
                qemu_set_irq(b->irq, 0);
            }
        }
    }
    if (env_true("QEMU_IPQ_QPIC_LOG") && off == QB_IRQ_SRCS_EE) {
        qpic_log("qpic-bam: read %#x -> %08x%s\n", (uint32_t)off,
                 qpic_bam_irq_srcs(b),
                 env_u32("QEMU_IPQ_QPIC_LOG", 0) > 1 ? trace_pc() : "");
    }
    switch (off) {
    case QB_CTRL:        return b->ctrl;
    case QB_DESC_TRSHLD: return b->desc_trshld;
    case QB_CNFG_BITS:   return b->cfg_bits;
    case QB_IRQ_STTS:    return b->irq_stts;
    case QB_IRQ_EN:      return b->irq_en;
    case QB_REVISION:
        /*
         * {major:8 at 23:16, minor:8 at 15:12, num_ees:4 at 11:8, step:8}.
         * The EE count is not decoration: Linux's bam_init() range-checks the
         * node's qcom,ee against it and fails the probe with -EINVAL when it
         * reads zero, so the QPIC's BAM has to report the same four EEs the
         * BLSP1 one does (bam_seed_id()).
         */
        return 0x00010407u;                     /* v1.0, 4 EEs, step 7 */
    case QB_NUM_PIPES:   return QB_PIPES;
    case QB_IRQ_SRCS:    return qpic_bam_irq_srcs(b);
    case QB_IRQ_SRCS_MSK:return b->irq_srcs_msk;
    case QB_IRQ_SRCS_EE: return qpic_bam_irq_srcs(b);
    case QB_IRQ_SRCS_MSK_EE: return b->irq_srcs_msk_ee;
    default:             return 0;
    }
}

static void qpic_bam_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    IPQQpicBam *b = opaque;
    int n = -1;
    uint32_t roff = 0;

    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        qpic_log("qpic-bam: write %#x <- %08x%s\n",
                      (uint32_t)off, (uint32_t)val,
                      env_u32("QEMU_IPQ_QPIC_LOG", 0) > 1 ? trace_pc() : "");
    }
    if (off >= QB_PIPE_BASE) {
        uint32_t rel = off - QB_PIPE_BASE;

        n = rel / QB_PIPE_STRIDE;
        roff = rel % QB_PIPE_STRIDE;
        if (n >= QB_PIPES) {
            return;
        }
    }
    if (n >= 0) {
        IPQQpicBamPipe *p = &b->pipe[n];

        switch (roff) {
        case QB_P_CTRL:      p->ctrl = (uint32_t)val; break;
        case QB_P_RST:
            if (val & 1) {
                p->cons = 0;
                p->irq_stts = 0;
                qpic_bam_update_irq(b);
            }
            break;
        case QB_P_IRQ_CLR:
            p->irq_stts &= ~(uint32_t)val;
            qpic_bam_update_irq(b);
            break;
        case QB_P_IRQ_EN:
            p->irq_en = (uint32_t)val;
            qpic_bam_update_irq(b);
            break;
        case QB_P_EVNT_REG:
            p->evnt_reg = (uint32_t)val;
            qpic_bam_run(b, n, (uint32_t)val);
            break;
        case QB_P_DESC_FIFO: p->desc_fifo = (uint32_t)val; break;
        case QB_P_FIFO_SIZES:p->fifo_sizes = (uint32_t)val; break;
        case QB_P_DATA_FIFO: p->data_fifo = (uint32_t)val; break;
        case QB_P_EVNT_TRSHLD: p->evnt_trshld = (uint32_t)val; break;
        case QB_P_EVNT_DEST: p->evnt_dest = (uint32_t)val; break;
        default: break;
        }
        return;
    }
    if (off >= IPQ_QPIC_BAM_SIZE) {
        unsigned idx = (off - IPQ_QPIC_BAM_SIZE) / 4;

        if (idx < ARRAY_SIZE(b->ext)) {
            b->ext[idx] = (uint32_t)val;
        }
        return;
    }
    switch (off) {
    case QB_CTRL:
        /*
         * Bit 0 is the BAM's software reset: it clears the pipes and the
         * global state, and it clears itself.  Storing it verbatim left pipe 1
         * reading back ENABLE from the previous boot, and MikroTik's SPS layer
         * refuses to connect a pipe that looks owned by someone else -- "BAM
         * 0x07984000 pipe 1 sharing violation", a failed msm_nand_probe, and
         * the installer saying "NAND is not detected" without ever touching
         * the controller.
         */
        if (val & 1) {
            int i;

            for (i = 0; i < QB_PIPES; i++) {
                memset(&b->pipe[i], 0, sizeof(b->pipe[i]));
            }
            b->desc_trshld = 0;
            b->cfg_bits = 0x0fffffff;
            b->irq_stts = 0;
            b->irq_en = 0;
            b->irq_srcs_msk = 0;
            b->irq_srcs_msk_ee = 0;
            qpic_bam_update_irq(b);
        }
        b->ctrl = (uint32_t)val & ~1u;          /* the reset bit self-clears */
        break;
    case QB_DESC_TRSHLD: b->desc_trshld = (uint32_t)val; break;
    case QB_CNFG_BITS:   b->cfg_bits = (uint32_t)val; break;
    case QB_IRQ_CLR:
        b->irq_stts &= ~(uint32_t)val;
        qpic_bam_update_irq(b);
        break;
    case QB_IRQ_EN:
        b->irq_en = (uint32_t)val;
        qpic_bam_update_irq(b);
        break;
    case QB_IRQ_SRCS_MSK:
        b->irq_srcs_msk = (uint32_t)val;
        qpic_bam_update_irq(b);
        break;
    case QB_IRQ_SRCS_MSK_EE:
        b->irq_srcs_msk_ee = (uint32_t)val;
        qpic_bam_update_irq(b);
        break;
    default: break;
    }
}

/*
 * Every BAM read is logged: the SPS layer decides whether a pipe may be
 * connected from what it reads back here, and "pipe 1 sharing violation" with
 * no trace of the reads is a decision you cannot argue with.
 */
static uint64_t qpic_bam_read(void *opaque, hwaddr off, unsigned size)
{
    uint64_t v = qpic_bam_read_raw(opaque, off, size);

    qpic_log("qpic-bam: read %#x -> %08x\n", (uint32_t)off, (uint32_t)v);
    return v;
}

static const MemoryRegionOps qpic_bam_ops = {
    .read = qpic_bam_read,
    .write = qpic_bam_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---- building the board's part ---------------------------------------- */

/*
 * Programming source for a die that does not hold one yet -- `-kernel` when the
 * boot chain comes from the die, or a board's own image on a blank part.  Two
 * shapes are accepted, because both are what ends up on disk:
 *
 *   * a whole-chip image, pages with the spare interleaved (`nanddump -o`, or
 *     `./nanddie.py to-die`), and
 *   * a data-only image, the page data with no spare at all (`cat /dev/mtdN`,
 *     `nanddump` without -o, and RouterBOOT's own NAND dumps).
 *
 * The length decides, so no caller has to declare which one it has: an image of
 * exactly the part's page data, or one that is not a whole number of stride
 * records, is data-only; anything else that divides by the stride is a chip
 * image.  Only page data is copied either way -- the destination's spare area
 * stays erased, which is what a bootloader that writes pages leaves behind.
 */
static void ipq_die_program_image(uint8_t *data, uint32_t bytes,
                                  const char *path, const char *what)
{
    uint32_t pages = bytes / NAND_STRIDE;
    gchar *buf = NULL;
    gsize len = 0, p;
    bool chip_layout;

    if (!path || !*path || !g_file_get_contents(path, &buf, &len, NULL)) {
        return;
    }
    chip_layout = len % NAND_STRIDE == 0 && len != (gsize)pages * NAND_PAGE;

    for (p = 0; p < pages; p++) {
        gsize src = chip_layout ? p * NAND_STRIDE : p * NAND_PAGE;
        gsize avail = src < len ? len - src : 0;

        if (avail > NAND_PAGE) {
            avail = NAND_PAGE;
        }
        if (!avail) {
            break;
        }
        memcpy(data + p * NAND_STRIDE, buf + src, avail);
    }
    fprintf(stderr, "ipq4019: %s programmed from %s (%zu bytes, %s)\n",
            what, path, (size_t)len,
            chip_layout ? "chip layout, spare dropped" : "page data");
    g_free(buf);
}

/*
 * The controller resets with the machine; the die does not, because the die is
 * the part and the part is the store.  What has to go is everything the
 * previous boot left *inside the controller*: staged program bytes and a
 * queued read must not be carried into the next boot's first command, and the
 * step counters have to start over, or the first page access of the new boot
 * would continue the old one instead of starting at its first codeword.
 */
static void qpic_dev_reset(void)
{
    IPQQpic *q = ipq_board ? ipq_board->qpic : NULL;
    IPQQpicBam *b = ipq_qpic_bam;
    int i;

    if (!q) {
        return;
    }
    memset(q->reg, 0, sizeof(q->reg));
    memset(q->buf, 0xff, sizeof(q->buf));
    q->tx_len = 0;
    q->txn_wr = 0;
    q->tx_page = UINT32_MAX;
    q->tx_page_set = false;
    q->tx_seq = false;
    q->tx_used = 0;
    q->tx_lost = false;
    q->pgm_n = 0;
    q->rd_len = 0;
    q->rd_lost = false;
    q->rd_page = UINT32_MAX;
    q->txn_rd = 0;
    q->rx_n = 0;
    q->loc_last = false;
    q->last_page = UINT32_MAX;

    if (!b) {
        return;
    }
    b->ctrl = 0;
    b->desc_trshld = 0;
    b->cfg_bits = 0x0fffffff;
    b->irq_stts = 0;
    b->irq_en = 0;
    b->irq_srcs_msk = 0;
    b->irq_srcs_msk_ee = 0;
    b->irq_observed = 0;
    b->irq_line = 0;
    for (i = 0; i < QB_PIPES; i++) {
        memset(&b->pipe[i], 0, sizeof(b->pipe[i]));
    }
    qpic_bam_update_irq(b);
}

static void ipq_board_add_qpic_nand(MemoryRegion *sysmem)
{
    IPQParNand *d;
    IPQQpic *q;
    IPQQpicBam *b;
    uint32_t per_block = NAND_STRIDE * NAND_PPBLK;
    const char *what;
    bool fresh = false;

    if (!ipq_cfg.qpic_nand) {
        return;
    }
    if (ipq_cfg.qpic_nand_size < per_block ||
        ipq_cfg.qpic_nand_size % per_block) {
        error_report("ipq4019: qpic-nand-size=%u must be a whole number of "
                     "%u-byte erase blocks", ipq_cfg.qpic_nand_size, per_block);
        exit(1);
    }

    d = g_new0(IPQParNand, 1);
    d->bytes = ipq_cfg.qpic_nand_size;
    d->pages = d->bytes / NAND_STRIDE;
    d->id_len = ipq_parse_hex_bytes(ipq_cfg.qpic_nand_id ? ipq_cfg.qpic_nand_id
                                                         : "c2f1801d",
                                    d->id, sizeof(d->id), "qpic-nand-id");
    d->data = g_malloc(d->bytes);
    memset(d->data, 0xff, d->bytes);
    d->store.fd = -1;
    d->store.what = "qpic-nand";

    what = ipq_cfg.qpic_nand_file && *ipq_cfg.qpic_nand_file
         ? ipq_cfg.qpic_nand_file
         : ipq_cfg.qpic_nand_drive && *ipq_cfg.qpic_nand_drive
         ? ipq_cfg.qpic_nand_drive : NULL;
    if (what) {
        uint64_t len = ipq_die_store_bind(&d->store, ipq_cfg.qpic_nand_file,
                                          ipq_cfg.qpic_nand_drive, d->bytes,
                                          true);

        if (len) {
            if (!ipq_die_store_pread(&d->store, d->data, d->bytes, 0)) {
                error_report("ipq4019: reading the qpic-nand store %s failed",
                             what);
                exit(1);
            }
            ipq_qpic_store_loaded = true;
        } else {
            if (!ipq_die_store_writable(&d->store)) {
                error_report("ipq4019: qpic-nand store %s is empty and cannot "
                             "be written", what);
                exit(1);
            }
            fresh = true;
        }
    }
    /*
     * A blank store on a QPIC boot is programmed from -kernel, exactly like the
     * SPI NAND above; a store that already holds a die is the part.  This has
     * to happen before the fresh die is written out, or the store keeps the
     * erased image.
     */
    if (ipq_cfg.qpic_nand_image && *ipq_cfg.qpic_nand_image &&
        (!ipq_die_store_open(&d->store) || fresh)) {
        ipq_die_program_image(d->data, d->bytes, ipq_cfg.qpic_nand_image,
                              "qpic-nand die");
    }
    if (fresh) {
        ipq_die_store_pwrite(&d->store, d->data, d->bytes, 0);
        ipq_die_store_sync(&d->store);
    }

    q = g_new0(IPQQpic, 1);
    q->die = d;
    q->last_page = UINT32_MAX;
    memory_region_init_io(&q->mr, NULL, &qpic_ops, q, "qpic-nand",
                          IPQ_QPIC_NANDC_SIZE);
    /*
     * The decode-only QPIC stub is installed later in the machine's init (it
     * covers the whole 0x40000 window), so these two live *inside* it and have
     * to outrank it.
     */
    memory_region_add_subregion_overlap(sysmem, IPQ_QPIC_NANDC_BASE, &q->mr, 1);

    b = g_new0(IPQQpicBam, 1);
    b->cfg_bits = 0x0fffffff;
    b->irq_srcs_msk_ee = 0;
    memory_region_init_io(&b->mr, NULL, &qpic_bam_ops, b, "qpic-bam",
                          IPQ_QPIC_BAM_SIZE);
    memory_region_add_subregion_overlap(sysmem, IPQ_QPIC_BAM_BASE, &b->mr, 1);
    ipq_qpic_bam = b;

    ipq_board->qpic = q;

    if (d->bytes % MiB) {
        fprintf(stderr, "ipq4019: qpic-nand %u bytes, id", d->bytes);
    } else {
        fprintf(stderr, "ipq4019: qpic-nand %u MiB, id",
                (unsigned)(d->bytes / MiB));
    }
    for (int i = 0; i < d->id_len; i++) {
        fprintf(stderr, " %02x", d->id[i]);
    }
    fprintf(stderr, ", %u pages of %u+%u, %u blocks%s\n", d->pages,
            NAND_PAGE, NAND_OOB, d->pages / NAND_PPBLK,
            ipq_die_store_open(&d->store) ? " <- the store" : " (RAM only)");
    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        qpic_log("qpic: nandc at %#x, bam at %#x\n",
                      IPQ_QPIC_NANDC_BASE, IPQ_QPIC_BAM_BASE);
    }
}


/*
 * The SPI NOR on chip select 0, as the board configuration describes it.
 *
 * Three things are the board's, not the SoC's: whether the part exists at all,
 * its die geometry and RDID, and the file that *is* the part when the board
 * has to keep its programming across a power cut.  The dump is only the
 * programming source for a fresh die -- once the die file holds a board, the
 * dump is not read, which is what a board with no programmer attached looks
 * like.
 */
static void ipq_board_add_nor(MemoryRegion *sysmem)
{
    const char *img;
    IPQMem *flash;
    gchar *buf = NULL;
    gsize len = 0;
    int pers;
    unsigned int j;

    if (!ipq_cfg.nor) {
        return;
    }

    flash = add_mem(sysmem, IPQ_FLASH_BASE, IPQ_FLASH_SIZE, "flash");
    ipq_board->nor = flash;

    ipq_flash_die_size = ipq_cfg.nor_size;
    ipq_flash_jedec_len = ipq_parse_hex_bytes(ipq_cfg.nor_id,
                                              ipq_flash_jedec,
                                              sizeof(ipq_flash_jedec),
                                              "nor-id");
    ipq_flash_uid_len = ipq_parse_hex_bytes(ipq_cfg.nor_uid,
                                            ipq_flash_uid,
                                            sizeof(ipq_flash_uid),
                                            "nor-uid");

    /* Unwritten flash reads 0xff, not 0x00. */
    memset(flash->data, 0xff, flash->size);

    img = ipq_cfg.nor_image;
    pers = nor_persist_preload(flash, ipq_flash_die_size, img,
                               ipq_cfg.nor_file, ipq_cfg.nor_drive);
    ipq_nor_store_loaded = (pers == NOR_PERSIST_LOADED);
    if (pers != NOR_PERSIST_LOADED) {
        if (img) {
            g_file_get_contents(img, &buf, &len, NULL);
        }
        if (buf) {
            memcpy(flash->data, buf, MIN(len, flash->size));
            g_free(buf);
        }
    }
    fprintf(stderr, "ipq4019: flash die %u MiB, RDID",
            (unsigned)(ipq_flash_die_size / MiB));
    for (j = 0; j < ipq_flash_jedec_len; j++) {
        fprintf(stderr, " %02x", ipq_flash_jedec[j]);
    }
    if (pers == NOR_PERSIST_LOADED) {
        fprintf(stderr, ", window %u MiB <- %s\n",
                (unsigned)(IPQ_FLASH_SIZE / MiB),
                ipq_cfg.nor_file ? ipq_cfg.nor_file : ipq_cfg.nor_drive);
    } else {
        fprintf(stderr, ", window %u MiB <- %s (%zu bytes)\n",
                (unsigned)(IPQ_FLASH_SIZE / MiB), img ? img : "(none)", len);
    }
    if (pers == NOR_PERSIST_EMPTY) {
        nor_persist_snapshot(ipq_flash_die_size);
    }
}

/*
 * The SPI NAND on chip select 1, as the board configuration describes it.
 * Absent unless the board has one: a part nobody probes still costs its whole
 * die in host RAM (132 MiB for the MX35LF1GE4AB), which is why the default
 * preset has no NAND.
 */
static void ipq_board_add_nand(void)
{
    uint32_t per_block = NAND_STRIDE * NAND_PPBLK;
    const char *what = ipq_cfg.nand_file && *ipq_cfg.nand_file
                     ? ipq_cfg.nand_file
                     : ipq_cfg.nand_drive && *ipq_cfg.nand_drive
                     ? ipq_cfg.nand_drive : NULL;
    bool fresh = false;
    IPQNand *n;
    int id_len;

    if (!ipq_cfg.nand) {
        return;
    }
    if (!ipq_cfg.nand_size) {
        ipq_cfg.nand_size = NAND_BYTES_DEF;
    }
    if (ipq_cfg.nand_size < per_block || ipq_cfg.nand_size % per_block) {
        error_report("ipq4019: nand-size=%u must be a whole number of "
                     "%u-byte erase blocks", ipq_cfg.nand_size, per_block);
        exit(1);
    }

    n = g_new0(IPQNand, 1);
    n->bytes = ipq_cfg.nand_size;
    n->pages = n->bytes / NAND_STRIDE;
    id_len = ipq_parse_hex_bytes(ipq_cfg.nand_id ? ipq_cfg.nand_id : "c212",
                                 n->id, sizeof(n->id), "nand-id");
    n->id_len = id_len;

    n->data = g_malloc(n->bytes);
    nand_fill_erased(n->data, n->bytes);
    nand_fill_erased(n->cache, NAND_STRIDE);
    n->status = 0;
    n->store.fd = -1;
    n->store.what = "nand";
    ipq_board->nand = n;

    /*
     * A store is the part: if it already holds a die, that is the NAND, and an
     * image is not needed.  Otherwise the store is created (erased) and, when
     * the board names one, filled from the image.
     */
    if (what) {
        fresh = !nand_persist_open(n, ipq_cfg.nand_file, ipq_cfg.nand_drive,
                                   ipq_cfg.nand_image);
        ipq_nand_store_loaded = !fresh;
        fprintf(stderr, "ipq4019: nand store %s %s (%u MiB, %u pages of %u+%u)\n",
                what, fresh ? "created, erased" : "loaded",
                (unsigned)(n->bytes / MiB), n->pages, NAND_PAGE, NAND_OOB);
    }

    /*
     * The image is content for a die that is not one yet: the board's own
     * image when the machine starts from a blank part, or -kernel when the
     * boot chain comes from the NAND.  A store that already holds a die is
     * the part, so nothing is programmed over it.
     */
    if (ipq_cfg.nand_image && *ipq_cfg.nand_image &&
        (!ipq_die_store_open(&n->store) || fresh)) {
        ipq_die_program_image(n->data, n->bytes, ipq_cfg.nand_image,
                              "nand die");
    }
    if (fresh) {
        nand_persist_snapshot(n);
    }
    fprintf(stderr, "ipq4019: nand die %u MiB, RDID %02x %02x, %u pages of %u+%u, "
            "%u blocks%s\n", (unsigned)(n->bytes / MiB), n->id[0], n->id[1],
            n->pages, NAND_PAGE, NAND_OOB, n->pages / NAND_PPBLK,
            ipq_die_store_open(&n->store) ? " <- the store" : " (RAM only)");
}

static void qup_spi_opcode(IPQQupl *q, uint8_t op)
{
    uint8_t prev_op = q->opcode;

    q->opcode = op;
    q->addr_left = 0;
    q->dummy_left = 0;
    q->read_data = 0;
    q->write_sr = 0;
    q->pgm_left = 0;
    q->wr_kind = NOR_WR_NONE;
    q->rx_len = 0;
    q->rx_pos = 0;
    q->rx_off = 0;
    memset(q->rx_buf, 0, sizeof(q->rx_buf));

    if (env_true("QEMU_IPQ_SPI_LOG")) {
        qemu_log_mask(LOG_UNIMP, "spi-op 0x%02x (prev 0x%02x addr 0x%06x len %u)\n",
                      op, prev_op, q->last_addr, q->data_bytes);
    }
    q->data_bytes = 0;

    if (q->target == NAND_CS_NAND && ipq_board->nand) {
        nand_opcode(q, op);
        return;
    }

    switch (op) {
    case 0x03:                          /* READ */
        q->addr_left = q->addr4 ? 4 : 3;
        q->read_data = 1;
        break;
    case 0x13:                          /* READ4B: always 32-bit address */
        /*
         * RouterBOOT picks this opcode whenever the offset does not fit in 24
         * bits (see its flash_read() helper: it tests addr >> 24), so the
         * address phase is four bytes, not three.  Getting this wrong shifts
         * every read: the kernel scan at 0x01000400 turned into a read of
         * 0x00010004 and never found the header.
         */
        q->addr_left = 4;
        q->read_data = 1;
        break;
    case 0x0b:                          /* FAST_READ */
        q->addr_left = q->addr4 ? 4 : 3;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0x0c:                          /* FAST_READ4 */
        q->addr_left = 4;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0x6b:                          /* 1-4-4 FAST_READ */
    case 0xeb:                          /* 1-4-4 FAST_READ (8 dummy) */
    case 0xbb:                          /* 1-1-4 FAST_READ */
        q->addr_left = q->addr4 ? 4 : 3;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0x5a:                          /* SFDP: address + 1 dummy byte */
        q->addr_left = q->addr4 ? 4 : 3;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0xb7:                          /* Enter 4-Byte Address Mode */
        q->addr4 = 1;
        break;
    case 0xe9:                          /* Exit 4-Byte Address Mode */
        q->addr4 = 0;
        break;
    case 0x9f:                          /* RDID */
        /*
         * Whatever die the board has: QEMU_IPQ_FLASH_JEDEC/_SIZE.  The part
         * repeats its id once it has shifted all of it out (the board's NOR
         * reads back as C22019C2 over four bytes), so a driver that asks for
         * more than three gets the id again rather than zeros.
         */
        memcpy(q->rx_buf, ipq_flash_jedec, ipq_flash_jedec_len);
        q->rx_len = ipq_flash_jedec_len;
        q->rx_wrap = 1;
        break;
    case 0x4b:                          /* Read Unique ID */
        /*
         * Winbond's format, the way MikroTik's flash_get_uid() reads it: the
         * command clocks four bytes out (the opcode plus three dummies) and
         * then reads nine in, skipping the first (the fourth dummy) and
         * taking the next eight as the id.  Without it the module falls back
         * to get_random_bytes, and the software id changes on every boot.
         * The id is the part's identity: stable, board-selectable.
         */
        q->dummy_left = 3;
        q->rx_buf[0] = 0x00;
        memcpy(q->rx_buf + 1, ipq_flash_uid, ipq_flash_uid_len);
        q->rx_len = 1 + ipq_flash_uid_len;
        break;
    case 0x05:                          /* RDSR1 */
        /*
         * The die is fast enough to finish between commands, so it reports
         * "busy" for exactly one read after a program/erase: a driver that
         * polls SR1 has to go round the loop, and one that does not is not
         * penalised by a status bit that never clears.
         */
        q->rx_buf[0] = q->status | (q->wip_pending ? 0x01 : 0);
        q->wip_pending = 0;
        q->rx_len = 1;
        break;
    case 0x35:                          /* RDSR2 */
        q->rx_buf[0] = q->status2;
        q->rx_len = 1;
        break;
    case 0x15:                          /* RDCR / RDSR3 */
        q->rx_buf[0] = 0x00;
        q->rx_len = 1;
        break;
    case 0x06:                          /* WREN */
    case 0x50:                          /* WREN for volatile status bits */
        q->status |= 0x02;              /* WEL */
        break;
    case 0x04:                          /* WRDI */
        q->status &= ~0x02;
        break;
    case 0x01:                          /* WRSR: SR1 [, SR2 [, SR3]] */
    case 0x31:
        q->write_sr = 3;
        break;
    case 0x02:                          /* PP, 256-byte page program */
    case 0x12:                          /* PP with a 4-byte address */
        q->addr_left = (op == 0x12) ? 4 : (q->addr4 ? 4 : 3);
        q->wr_kind = NOR_WR_PP;
        break;
    case 0x20:                          /* SE: 4 KiB sector erase */
    case 0x21:                          /* SE with a 4-byte address */
        q->addr_left = (op == 0x21) ? 4 : (q->addr4 ? 4 : 3);
        q->wr_kind = NOR_WR_SE4K;
        break;
    case 0x52:                          /* BE32 */
    case 0x5c:
        q->addr_left = (op == 0x5c) ? 4 : (q->addr4 ? 4 : 3);
        q->wr_kind = NOR_WR_BE32;
        break;
    case 0xd8:                          /* BE64 */
    case 0xdc:
        q->addr_left = (op == 0xdc) ? 4 : (q->addr4 ? 4 : 3);
        q->wr_kind = NOR_WR_BE64;
        break;
    case 0xc7:                          /* CE, no address phase */
    case 0x60:
        q->wr_kind = NOR_WR_CE;
        q->addr = 0;
        nor_write_start(q);
        break;
    default:
        /* unknown opcode: no data phase, accept and ignore */
        break;
    }
}

static void qup_spi_byte(IPQQupl *q, uint8_t b)
{
    if (env_u32("QEMU_IPQ_SPI_LOG", 0) > 1) {
        qemu_log_mask(LOG_UNIMP, "spi-byte %02x phase %d\n", b, q->phase);
    }
    switch (q->phase) {
    case 0:
        qup_spi_opcode(q, b);
        if (q->addr_left) {
            q->phase = 1;
        } else if (q->dummy_left) {
            q->phase = 2;
        } else {
            q->phase = 3;
        }
        break;
    case 1:
        q->addr = (q->addr << 8) | b;
        if (--q->addr_left == 0) {
            q->rx_off = q->addr;
            q->last_addr = q->addr;
            q->phase = q->dummy_left ? 2 : 3;
            if (q->target == NAND_CS_NAND && ipq_board->nand) {
                nand_address_done(q);
            } else if (q->wr_kind) {
                nor_write_start(q);
            }
            die_dbg_n = 0;
            if (die_dbg_on()) {
                die_dbg_log("die: xact op=0x%02x addr=0x%08x pc=%08x\n",
                            q->opcode, q->addr, die_dbg_pc());
            }
            if (env_true("QEMU_IPQ_SPI_LOG")) {
                qemu_log_mask(LOG_UNIMP, "spi-cmd 0x%02x addr 0x%06x\n",
                              q->opcode, q->addr);
            }
        }
        break;
    case 2:
        if (--q->dummy_left == 0) {
            q->phase = 3;
        }
        break;
    default:
        /*
         * Data phase.  Three things can be in flight: a status-register
         * write (SBL1 clears a block-protect bit during init and reads it
         * back), a page program, or nothing at all.
         */
        if (q->target == NAND_CS_NAND && ipq_board->nand) {
            nand_data_byte(b);
        } else if (q->write_sr) {
            int idx = 3 - q->write_sr;

            if (idx == 0) {
                q->status = b & ~0x02;  /* WEL self-clears */
            } else if (idx == 1) {
                q->status2 = b;
            }
            q->write_sr--;
        } else if (q->pgm_left) {
            nor_program_byte(q, b);
        }
        break;
    }
}

/* MISO level for the byte being clocked now, i.e. the state it starts in */
static uint8_t qup_miso(IPQQupl *q)
{
    if (q->phase != 3) {
        return 0x00;
    }
    if (q->target == NAND_CS_NAND && ipq_board->nand) {
        return nand_miso(q);
    }
    if (q->read_data) {
        uint8_t v = qup_flash_byte(q->rx_off++ & (ipq_flash_die_size - 1));

        q->data_bytes++;
        die_dbg_note(v);
        return v;
    }
    if (q->rx_pos < q->rx_len) {
        return q->rx_buf[q->rx_pos++];
    }
    if (q->rx_wrap && q->rx_len > 0) {
        return q->rx_buf[q->rx_pos++ % q->rx_len];
    }
    return 0x00;
}

/* One byte out on MOSI means one byte in on MISO, as on real hardware. */
void qup_clock(IPQQupl *q, uint8_t mosi)
{
    uint8_t miso = qup_miso(q);

    if (die_dbg_on() && q->phase <= 2) {
        die_dbg_log("die: mosi %02x phase=%d pc=%08x\n", mosi, q->phase,
                    die_dbg_pc());
    }

    if (q->resp_head + q->resp_len < (int)q->resp_cap) {
        q->resp[q->resp_head + q->resp_len] = miso;
        q->resp_len++;
    }
    q->clocked++;
    q->clocks++;
    qup_spi_byte(q, mosi);
}

/*
 * Bytes per SPI word, from QUP_CONFIG[4:0] (bits_per_word - 1, as
 * spi_qup_io_config() programs it).  QCA's boot drivers never touch the
 * register, so the reset value has to mean one byte per word too.
 */
static uint32_t qup_wsize(IPQQupl *q)
{
    return DIV_ROUND_UP((q->shadow[QUP_REG_CONFIG / 4] & 0x1f) + 1, 8);
}

/* Data direction field of QUP_IO_M_MODES: 0 FIFO, 1 BLOCK, 2 DMOV, 3 BAM */
static uint32_t qup_mode(IPQQupl *q)
{
    return (q->shadow[QUP_REG_IO_MODES / 4] >> 10) & 0x3;
}

/* How many SPI words this transfer is programmed to carry, in each direction */
static uint32_t qup_exp_words(IPQQupl *q, int tx)
{
    switch (qup_mode(q)) {
    case QUP_IO_M_MODE_FIFO:
        return q->shadow[(tx ? QUP_REG_MX_WRITE : QUP_REG_MX_READ) / 4];
    case QUP_IO_M_MODE_BLOCK:
        return q->shadow[(tx ? QUP_REG_MX_OUTPUT : QUP_REG_MX_INPUT) / 4];
    default:
        /* BAM: only the input count survives the driver's programming */
        return tx ? 0 : q->shadow[QUP_REG_MX_INPUT / 4];
    }
}

/*
 * QUP_OPERATIONAL.
 *
 * The FIFO status bits are level and derived from the current occupancy --
 * this model clocks a byte the moment software hands it to the output FIFO,
 * so "room left" is always true and the input FIFO holds whatever the die
 * shifted out and nobody has read yet.  MAX_*_DONE are sticky events, and
 * the interrupt line is the OR of the unmasked service flags plus
 * MAX_INPUT_DONE (see qup_update_irq()).
 */
static uint32_t qup_operational(IPQQupl *q)
{
    uint32_t ws = qup_wsize(q);
    uint32_t mode = qup_mode(q);
    uint32_t exp_tx = qup_exp_words(q, 1) * ws;
    uint32_t v = 0;
    int running = q->state == QUP_STATE_RUN;

    /*
     * FIFO occupancy is reported whatever the state machine is doing.  QCA's
     * boot drivers poll it straight after writing a byte and never program
     * QUP_STATE at all, so gating it on RUN would hang the flash probe.
     */
    if (q->resp_len >= (int)ws) {
        v |= QUP_OP_IN_FIFO_NOT_EMPTY;
        if (mode == QUP_IO_M_MODE_BLOCK) {
            v |= QUP_OP_IN_BLOCK_READ_REQ;
        }
    }
    if (running && q->tx_bytes < exp_tx) {
        v |= QUP_OP_OUT_SERVICE_FLAG;
        if (mode == QUP_IO_M_MODE_BLOCK) {
            v |= QUP_OP_OUT_BLOCK_WRITE_REQ;
        }
    }
    if (running && q->resp_len >= (int)ws) {
        v |= QUP_OP_IN_SERVICE_FLAG;
    }
    if (q->max_out_done) {
        v |= QUP_OP_MAX_OUTPUT_DONE;
    }
    if (q->max_in_done) {
        v |= QUP_OP_MAX_INPUT_DONE;
    }
    return v;
}

/*
 * Level-sensitive interrupt: asserted while an unmasked service flag or
 * MAX_INPUT_DONE is pending.  MAX_OUTPUT_DONE is deliberately not wired --
 * the driver never looks at it, and in PIO mode it stays set after the last
 * byte was queued, which would leave the line stuck high.
 */
static void qup_update_irq(IPQQupl *q)
{
    uint32_t op = qup_operational(q);
    uint32_t bits = op & (QUP_OP_IN_SERVICE_FLAG | QUP_OP_OUT_SERVICE_FLAG |
                          QUP_OP_MAX_INPUT_DONE);

    qemu_set_irq(q->irq, (bits & ~q->op_mask) != 0);
}

/* Raise the sticky completion bits once the programmed counts are reached */
static void qup_update_done(IPQQupl *q)
{
    uint32_t ws = qup_wsize(q);
    uint32_t exp_tx = qup_exp_words(q, 1) * ws;
    uint32_t exp_rx = qup_exp_words(q, 0) * ws;

    if (q->state != QUP_STATE_RUN) {
        return;
    }
    if (exp_tx && q->tx_bytes >= exp_tx) {
        q->max_out_done = 1;
    }
    /*
     * MAX_INPUT_DONE is only an interrupt source when the service flag is
     * masked out, i.e. in BAM/DMOV mode -- that is the only completion
     * signal dma transfers get from the QUP.  In PIO mode the driver reads
     * the FIFO on IN_SERVICE_FLAG and completes there, and a sticky
     * MAX_INPUT_DONE it never clears would leave the line stuck high.
     */
    if (exp_rx && q->clocks >= exp_rx &&
        (q->op_mask & QUP_OP_IN_SERVICE_FLAG)) {
        q->max_in_done = 1;
    }
}

/* One SPI word out of the input FIFO, bytes packed big-endian in the word */
uint32_t qup_in_word(IPQQupl *q)
{
    uint32_t ws = qup_wsize(q);
    uint32_t v = 0;
    uint32_t i;

    if (q->resp_len <= 0) {
        return 0xff;                 /* FIFO idle level */
    }
    for (i = 0; i < ws; i++) {
        uint8_t b = 0xff;

        if (q->resp_len > 0) {
            b = q->resp[q->resp_head++];
            q->resp_len--;
            q->rx_bytes++;
        }
        /*
         * Linux's spi_qup_read_from_fifo() pulls byte i from shift
         * 8 * (w_size - i - 1), so the word carries w_size bytes
         * big-endian; with one byte per word that is the low byte, which
         * is also the lane QCA's boot drivers read.
         */
        v |= (uint32_t)b << (8 * (ws - 1 - i));
    }
    if (q->resp_len == 0) {
        q->resp_head = 0;
        /* MAX_INPUT_DONE is an event: it clears once the FIFO is drained */
        q->max_in_done = 0;
    }
    return v;
}

static uint64_t qupl_read(void *opaque, hwaddr off, unsigned size)
{
    IPQQupl *q = opaque;
    uint32_t idx = off >> 2;
    uint32_t v;

    switch (off) {
    case QUP_REG_STATE:
        /* QUP_STATE_VALID is set by the hardware once a write settles */
        v = (idx < ARRAY_SIZE(q->shadow) ? q->shadow[idx] : 0) | 0x4;
        break;
    case QUP_REG_OPERATIONAL:
        v = qup_operational(q);
        break;
    case QUP_REG_ERROR_FLAGS:
    case 0x308:                    /* SPI_ERROR_FLAGS: no clock under/overrun */
        v = 0;
        break;
    case 0x30c:                    /* SPI_ERROR_FLAGS_EN, writable */
    case QUP_REG_CONFIG:
    case QUP_REG_IO_MODES:
    case QUP_REG_OP_MASK:
    case QUP_REG_MX_OUTPUT:
    case QUP_REG_MX_WRITE:
    case QUP_REG_MX_INPUT:
    case QUP_REG_MX_READ:
        v = idx < ARRAY_SIZE(q->shadow) ? q->shadow[idx] : 0;
        break;
    case QUP_REG_SW_RESET:
        v = 0;                     /* self-clearing */
        break;
    case QUP_IN_FIFO:
    case QUP_IN_FIFO + 4:
    case QUP_IN_FIFO + 8:
    case QUP_IN_FIFO + 0xc:
        v = qup_in_word(q);
        qup_update_irq(q);
        break;
    default:
        v = idx < ARRAY_SIZE(q->shadow) ? q->shadow[idx] : 0;
        break;
    }
    if (env_true("QEMU_IPQ_TRACE") || env_true("QEMU_IPQ_QUP_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "qup-spi: read  off 0x%03" HWADDR_PRIx
                      " -> 0x%08x%s\n", off, v,
                      (env_u32("QEMU_IPQ_TRACE", 0) > 1 ||
                       env_true("QEMU_IPQ_QUP_TRACE")) ? trace_pc() : "");
    }
    return v;
}

/* Drop whatever the die shifted out but nobody read (input FIFO flush). */
static void qup_fifo_flush(IPQQupl *q)
{
    q->resp_head = 0;
    q->resp_len = 0;
}

/* Start of a new SPI transaction: back to the opcode phase, MISO empty. */
static void qup_bus_reset(IPQQupl *q)
{
    q->phase = 0;
    q->addr = 0;
    q->addr_left = 0;
    q->dummy_left = 0;
    q->read_data = 0;
    q->nand_read = 0;
    q->rx_off = 0;
    q->rx_len = 0;
    q->rx_pos = 0;
    q->rx_wrap = 0;
    qup_fifo_flush(q);
    nand_xact_reset();
}

/*
 * There are two dies and two ways to select them, and they are not
 * interchangeable.
 *
 *   - The QUP has one native chip select of its own, CS0, and QCA's boot
 *     drivers are its only users: they assert it with FORCE_CS in
 *     SPI_IO_CONTROL and never look at cs-gpios.  It reaches the NOR.
 *   - A board whose DTS lists cs-gpios selects each die from a TLMM pin
 *     (54 = CS0/NOR, 59 = CS1/NAND).  Linux's spi-qup keeps programming the
 *     QUP chip select as well -- spi_qup_set_cs() is installed even with
 *     use_gpio_descriptors -- but with the opposite convention (it sets
 *     FORCE_CS on *deassert*), and on this board that pin is not wired to
 *     either die.
 *
 * So the bus has exactly one owner: whichever source claimed it, and a
 * source may only claim it while it is free.  FORCE_CS therefore reaches the
 * NOR only when no cs-gpio holds a die -- which is the boot chain's case and
 * never the kernel's.
 */
#define CS_OWNER_NONE   0
#define CS_OWNER_FORCE  1
#define CS_OWNER_GPIO   2

static void qup_spi_select(IPQQupl *q, int which)
{
    if (die_dbg_on()) {
        die_dbg_log("die: select %s op=0x%02x addr=0x%08x n=%u"
                    " first=%02x %02x %02x %02x\n",
                    which == NAND_CS_NAND ? "nand" : "nor", q->opcode,
                    q->last_addr, die_dbg_n, die_dbg_first[0],
                    die_dbg_first[1], die_dbg_first[2], die_dbg_first[3]);
    }
    q->target = which;
    q->cs = 1;
    qup_bus_reset(q);
}

/* Whatever owned the bus let go: the die forgets the command in progress. */
static void qup_deselect(IPQQupl *q)
{
    q->cs = 0;
    q->cs_owner = CS_OWNER_NONE;
    qup_bus_reset(q);
}

static void qup_cs_force(IPQQupl *q, int level)
{
    if (q->cs_force == level) {
        return;
    }
    q->cs_force = level;
    if (level) {
        if (q->cs_owner == CS_OWNER_NONE) {
            q->cs_owner = CS_OWNER_FORCE;
            qup_spi_select(q, NAND_CS_NOR);
        }
    } else if (q->cs_owner == CS_OWNER_FORCE) {
        qup_deselect(q);
    }
}

static void qupl_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQQupl *q = opaque;
    uint32_t idx = off >> 2;

    if (die_dbg_on()) {
        die_dbg_log("die: qup-w off=0x%03x val=0x%08x size=%u cs=%d state=%u"
                    " pc=%08x\n",
                    (unsigned)off, (uint32_t)val, size, q->cs, q->state,
                    die_dbg_pc());
    }

    if (env_true("QEMU_IPQ_TRACE") || env_true("QEMU_IPQ_QUP_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "qup-spi: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", off, val,
                      (env_u32("QEMU_IPQ_TRACE", 0) > 1 ||
                       env_true("QEMU_IPQ_QUP_TRACE")) ? trace_pc() : "");
    }
    if (off == QUP_REG_SW_RESET) {
        qup_bus_reset(q);
        return;
    }
    if (off == QUP_REG_IO_CONTROL) {
        /*
         * The QCA driver asserts chip select with FORCE_CS in this register
         * (qca_qup_spi.c: write_force_cs) and keeps it asserted for the whole
         * spi_xfer(), so a CS edge -- not a QUP state write -- is what starts
         * and ends a die-level SPI transaction.
         */
        qup_cs_force(q, (val & QUP_IO_CONTROL_FORCE_CS) != 0);
    }
    if (off == QUP_REG_STATE) {
        uint32_t st = val & 0x3;

        if (st == QUP_STATE_CLEAR) {
            st = QUP_STATE_RESET;      /* PAUSE -> CLEAR -> RESET */
        }
        /*
         * The state field has to read back as the state that was requested:
         * readl(QUP_STATE) reports STATE_VALID (0x4) plus the low two bits,
         * and every driver that programs the engine spins on
         * ((readl(QUP_STATE) & 0x3) == requested) before touching the FIFOs.
         *
         * Leaving the read-back at a constant 0x4 makes that poll fail even
         * though the write was taken: the transfer never enters RUN, so
         * qup_operational() never raises the service flags and qup_update_done()
         * never sets the done bits.  RouterOS's flash.ko then retries the
         * rejected read forever, which is the `/system reboot` hang -- it also
         * keeps the reboot thread from ever reaching the pshold write.
         */
        q->shadow[QUP_REG_STATE / 4] = st;
        if (st == QUP_STATE_RESET) {
            /*
             * State RESET flushes the input FIFO and abandons the programmed
             * counts.  The die itself only loses the transaction when chip
             * select drops with it; while FORCE_CS holds CS low there is no
             * CS edge, so a reset between the command and data phase (U-Boot
             * does exactly that) keeps the command in progress -- only the
             * stale command-phase MISO bytes go.
             */
            q->state = QUP_STATE_RESET;
            q->tx_bytes = q->rx_bytes = q->clocks = 0;
            q->max_in_done = q->max_out_done = 0;
            qup_fifo_flush(q);
            if (!q->cs) {
                qup_bus_reset(q);
            }
        } else {
            q->state = st;
            qup_update_done(q);
        }
        qup_update_irq(q);
        return;
    }
    if (off == QUP_REG_OPERATIONAL) {
        /*
         * Write-one-to-clear for the event bits the ISR acknowledges:
         * IN_SERVICE_FLAG, OUT_SERVICE_FLAG and MAX_INPUT_DONE.  The level
         * bits (FIFO status, block requests) are not writable.
         */
        if (val & QUP_OP_IN_SERVICE_FLAG) {
            q->max_in_done = 0;
        }
        if (val & QUP_OP_MAX_INPUT_DONE) {
            q->max_in_done = 0;
        }
        if (val & QUP_OP_MAX_OUTPUT_DONE) {
            q->max_out_done = 0;
        }
        qup_update_irq(q);
        return;
    }
    if (off == QUP_REG_OP_MASK) {
        q->op_mask = val;
        qup_update_irq(q);
        return;
    }
    if (off >= QUP_OUT_FIFO && off < QUP_OUT_FIFO + 0x40) {
        /*
         * Byte packing.  Two masters write here and they disagree about the
         * lane: QCA's boot drivers do `ldrb r3,[buf],#1; str r3,[fifo]` (byte
         * in 7:0), while Linux's spi_qup_write_to_fifo() shifts by
         * 8 * (3 - i) (byte in 31:24).  A low byte of zero with something in
         * the top byte can only be the Linux lane, so use that to tell them
         * apart; every other case is the QCA lane the boot chain needs.
         */
        uint32_t ws = qup_wsize(q);
        uint32_t i;

        if (ws == 1) {
            uint8_t b = ((val & 0xff) == 0 && (val & 0xff000000)) ?
                        (uint8_t)(val >> 24) : (uint8_t)val;

            if (die_dbg_on()) {
                die_dbg_log("die: fifo-tx 0x%08x -> byte 0x%02x\n",
                            (uint32_t)val, b);
            }
            qup_clock(q, b);
        } else {
            for (i = 0; i < ws; i++) {
                qup_clock(q, (val >> (8 * (3 - i))) & 0xff);
            }
        }
        q->tx_bytes += ws;
        qup_update_done(q);
        qup_update_irq(q);
        return;
    }
    if (idx < ARRAY_SIZE(q->shadow)) {
        q->shadow[idx] = val;
    }
    if (off == QUP_REG_CONFIG || off == QUP_REG_IO_MODES) {
        qup_update_done(q);
        qup_update_irq(q);
    }
}

static void ipq_cs_line(void *opaque, int level)
{
    unsigned which = (unsigned)(uintptr_t)opaque & 0xff;
    unsigned pin = (unsigned)(uintptr_t)opaque >> 8;
    IPQQupl *q = ipq->qup_spi;
    bool asserted = level == 0;

    if (!q) {
        return;
    }
    if (die_dbg_on()) {
        die_dbg_log("die: cs%u %s %s\n", pin,
                    asserted ? "assert" : "release",
                    which == NAND_CS_NAND ? "nand" : "nor");
    }
    if (asserted) {
        qup_spi_select(q, which);
        q->cs_owner = CS_OWNER_GPIO;
        q->cs_gpio_die = which;
    } else if (q->cs_owner == CS_OWNER_GPIO && q->cs_gpio_die == which) {
        qup_deselect(q);
    }
}

static const MemoryRegionOps qupl_ops = {
    .read = qupl_read,
    .write = qupl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static IPQQupl *add_qupl(MemoryRegion *parent, hwaddr base, hwaddr size,
                         const char *name)
{
    IPQQupl *q = g_new0(IPQQupl, 1);

    /*
     * A whole SPI_MAX_XFER (64 KiB) of MISO has to fit: in BLOCK mode the
     * driver hands the QUP every byte of a flash read in one go, and this
     * model shifts the reply into the queue as the bytes go out.
     */
    q->resp_cap = 1u << 17;
    q->resp = g_new0(uint8_t, q->resp_cap);
    /* QUP configuration registers come up cleared, not "nothing here" */
    memory_region_init_io(&q->mr, NULL, &qupl_ops, q, name, size);
    memory_region_add_subregion(parent, base, &q->mr);
    ipq->qup_spi = q;
    return q;
}

/* ------------------------------------------------------------------ */
/* Register-file block                                                 */
/* ------------------------------------------------------------------ */

/*
 * Many SoC blocks are plain register files: software writes a bit, later
 * reads it back; nothing in them models a bus transaction.  A zero-filled
 * array that returns what was written is the least surprising model for
 * those, and it is what the switch (0x0c000000) and EDMA (0x0c080000)
 * blocks need for their configuration/status registers.
 */
struct IPQRegFile {
    MemoryRegion mr;
    uint32_t *regs;
    uint32_t nregs;
    unsigned dbg;                   /* log accesses when QEMU_IPQ_ESS_LOG is set */
    /*
     * Registers holding a "busy" bit that hardware clears as soon as the
     * operation the write started has finished.  Software sets the bit and
     * then polls for it to read back 0; storing it verbatim makes every such
     * poll time out (the switch's MIB flush and ATU/VTU lookups do this).
     */
    struct {
        hwaddr off;
        uint32_t mask;
    } clr[8];
    int nclr;
    /*
     * The contents at the end of machine init, i.e. what the hardware reads
     * after power-on.  A system reset drops whatever the previous OS left
     * behind and restores this, not zeroes: registers whose value is
     * configuration (the switch's device id) have already been programmed by
     * the time the guest runs, and the reset it gets before its first
     * instruction must not wipe them.
     */
    uint32_t *por;
};

static uint64_t regfile_read(void *opaque, hwaddr off, unsigned size)
{
    IPQRegFile *r = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr o = off + i;
        uint32_t w = r->regs[(o >> 2) % r->nregs];
        v |= (uint64_t)((w >> ((o & 3) * 8)) & 0xff) << (i * 8);
    }
    if (r->dbg) {
        qemu_log_mask(LOG_UNIMP, "%s: read  0x%05" HWADDR_PRIx
                      " -> 0x%08" PRIx64 "%s\n",
                      memory_region_name(&r->mr), off, v, trace_pc());
    }
    return v;
}

static void regfile_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQRegFile *r = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr o = off + i;
        uint32_t *w = &r->regs[(o >> 2) % r->nregs];
        unsigned sh = (o & 3) * 8;
        *w = (*w & ~(0xffu << sh)) | (((val >> (i * 8)) & 0xff) << sh);
    }
    for (i = 0; i < r->nclr; i++) {
        if ((off & ~3u) == r->clr[i].off) {
            r->regs[r->clr[i].off >> 2] &= ~r->clr[i].mask;
        }
    }
    if (r->dbg) {
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%05" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n",
                      memory_region_name(&r->mr), off, val, trace_pc());
    }
}

static const MemoryRegionOps regfile_ops = {
    .read = regfile_read,
    .write = regfile_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* Declare a busy bit that clears itself once the write has taken effect. */
static void regfile_clr_bit(IPQRegFile *r, hwaddr off, uint32_t mask)
{
    assert(r->nclr < ARRAY_SIZE(r->clr));
    r->clr[r->nclr].off = off;
    r->clr[r->nclr].mask = mask;
    r->nclr++;
}

static IPQRegFile *add_regfile(MemoryRegion *parent, hwaddr base, hwaddr size,
                               const char *name)
{
    IPQRegFile *r = g_new0(IPQRegFile, 1);

    r->nregs = size / 4;
    r->regs = g_new0(uint32_t, r->nregs);
    r->dbg = getenv("QEMU_IPQ_ESS_LOG") != NULL;
    memory_region_init_io(&r->mr, NULL, &regfile_ops, r, name, size);
    memory_region_add_subregion(parent, base, &r->mr);

    /* Worth resetting to its power-on values on the next system reset. */
    if (!ipq->regfile_list) {
        ipq->regfile_list = g_ptr_array_new();
    }
    g_ptr_array_add(ipq->regfile_list, r);
    return r;
}

/* ------------------------------------------------------------------ */
/* qca-gcnt: 32-bit free running counter                               */
/* ------------------------------------------------------------------ */

static uint64_t gcnt_read(void *opaque, hwaddr off, unsigned size)
{
    uint32_t hz = *(uint32_t *)opaque;
    uint64_t ticks;

    ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                     ipq_counter_epoch_ns, hz, NANOSECONDS_PER_SECOND);
    /* Delay loops read this thousands of times a second: only at TRACE=3. */
    if (env_u32("QEMU_IPQ_TRACE", 0) > 2) {
        qemu_log_mask(LOG_UNIMP, "counter: read off 0x%x -> 0x%x%s\n",
                      (unsigned)off, (uint32_t)ticks, trace_pc());
    }
    if (off >= 8) {
        return 0;
    }
    return (uint32_t)(ticks >> (off * 8));
}

static void gcnt_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "gcnt: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "\n", off, val);
    }
}

static const MemoryRegionOps gcnt_ops = {
    .read = gcnt_read,
    .write = gcnt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */

static struct arm_boot_info ipq4019_binfo;

/* entry point of an ELF stage loaded from the flash dump, 0 = none */
static uint32_t ipq4019_elf_entry;

/*
 * SBL1's entry routine receives, in r0, a pointer to the boot-config struct
 * the PBL leaves in shared memory (boot_images sbl1_mc.c:226 asserts when it
 * is NULL).  We fabricate a minimal one: 0x54 bytes plus a 0x50-byte block
 * pointed to by +0x30.
 */
static uint32_t ipq4019_bootinfo;

typedef struct IPQRange {
    hwaddr base;
    hwaddr size;
} IPQRange;

/* every RAM window handed out by ipq_map_ram(), so ELF segments don't remap */

static bool ipq_range_covered(hwaddr base, hwaddr size)
{
    int i;

    if (!ipq->mapped) {
        return false;
    }
    for (i = 0; i < ipq->mapped->len; i++) {
        IPQRange *r = &g_array_index(ipq->mapped, IPQRange, i);

        if (base >= r->base && base + size <= r->base + r->size) {
            return true;
        }
    }
    return false;
}

static void ipq_map_ram(MemoryRegion *sysmem, hwaddr base, hwaddr size,
                        const char *name)
{
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    IPQRange r = { base, size };

    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(sysmem, base, mr);
    if (!ipq->mapped) {
        ipq->mapped = g_array_new(FALSE, FALSE, sizeof(IPQRange));
    }
    g_array_append_val(ipq->mapped, r);
}

/*
 * Load an ARM32 ELF boot stage (QCA SBL1) the way the PBL would: map RAM at
 * every PT_LOAD p_paddr and copy the segment there.  Returns false if the
 * file is not an ELF32/ARM image.
 */
/*
 * Boot stages.
 *
 * The boot flash holds a chain of ELF images: SBL1 first, the TZ/QSEE image it
 * loads next, and then the APPSBL that runs when TZ returns.  Each image
 * carries its own entry point, so the machine can read the handover addresses
 * out of the image it was given instead of being told three literals by the
 * harness.  The order in the flash is the order SBL1 walks them, which is why
 * the scan below is positional: first image at offset 0, next one after it,
 * next one after that.
 *
 * This is layout, not policy: nothing here knows or cares what the images do.
 */
#define IPQ_MAX_STAGES 3

typedef struct IPQBootStage {
    const char *name;
    uint32_t image_offset;      /* where the image starts in the boot image */
    uint32_t entry;             /* ELF e_entry: where the stage is started   */
    uint32_t load;              /* first PT_LOAD address, for the log        */
    uint32_t size;              /* its memsz, for the log                    */
} IPQBootStage;

static IPQBootStage ipq_boot_stages[IPQ_MAX_STAGES];
static int ipq_boot_stage_count;
static unsigned ipq_qsee_skips;

/*
 * Is there an ELF32/ARM executable image at @off?  On success fills in the
 * entry point and the first PT_LOAD's address and size.  Images embedded in a
 * flash dump are not necessarily aligned to anything, but they are word
 * aligned, so the caller walks the buffer four bytes at a time.
 */
static bool ipq_elf_image_at(const uint8_t *img, size_t len, size_t off,
                             uint32_t *entry, uint32_t *load, uint32_t *size)
{
    const Elf32_Ehdr *eh;
    uint32_t load_addr = 0, load_size = 0;
    int i;

    if (off + sizeof(*eh) > len) {
        return false;
    }
    eh = (const Elf32_Ehdr *)(img + off);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS32 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_type != ET_EXEC || eh->e_machine != EM_ARM ||
        eh->e_ehsize != sizeof(*eh) || eh->e_entry == 0) {
        return false;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize >
        len - off) {
        return false;
    }
    for (i = 0; i < eh->e_phnum; i++) {
        const Elf32_Phdr *ph = (const Elf32_Phdr *)(img + off + eh->e_phoff +
                                                    (size_t)i * eh->e_phentsize);

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        if (load_addr == 0 || ph->p_paddr < load_addr) {
            load_addr = ph->p_paddr;
            load_size = ph->p_memsz;
        }
    }
    *entry = eh->e_entry;
    *load = load_addr;
    *size = load_size;
    return true;
}

static void ipq4019_scan_stages(const uint8_t *img, size_t len)
{
    static const char *const names[IPQ_MAX_STAGES] = { "sbl1", "qsee", "appsbl" };
    uint32_t entry, load, size;
    size_t off;
    int i;

    ipq_boot_stage_count = 0;
    ipq->qsee_stage = ipq->appsbl_stage = NULL;

    if (!ipq_elf_image_at(img, len, 0, &entry, &load, &size)) {
        return;
    }
    ipq_boot_stages[0] = (IPQBootStage){ names[0], 0, entry, load, size };
    ipq_boot_stage_count = 1;

    /*
     * Every later image belongs to the DRAM the SoC maps at 0x80000000 (the
     * kernel images further down the flash do too, but the scan stops once
     * the boot chain is complete, long before them).
     */
    for (off = 4; off + sizeof(Elf32_Ehdr) <= len &&
                  ipq_boot_stage_count < IPQ_MAX_STAGES; off += 4) {
        if (!ipq_elf_image_at(img, len, off, &entry, &load, &size) ||
            entry < 0x80000000) {
            continue;
        }
        i = ipq_boot_stage_count;
        ipq_boot_stages[i] = (IPQBootStage){ names[i], off, entry, load, size };
        ipq_boot_stage_count++;
    }

    ipq->qsee_stage = ipq_boot_stage_count == 3 ? &ipq_boot_stages[1] : NULL;
    ipq->appsbl_stage = ipq_boot_stage_count == 3 ? &ipq_boot_stages[2] : NULL;

    fprintf(stderr, "ipq4019: stages:");
    for (i = 0; i < ipq_boot_stage_count; i++) {
        fprintf(stderr, " %s@0x%08x", ipq_boot_stages[i].name,
                ipq_boot_stages[i].entry);
    }
    if (ipq_boot_stage_count < IPQ_MAX_STAGES) {
        fprintf(stderr, " (boot chain incomplete)");
    }
    fprintf(stderr, "\n");
}

/*
 * QSEE policy.
 *
 * SBL1 branches into QSEE (the vendor TZ blob).  Its early bring-up performs
 * an access that it expects the QCOM protection fabric -- NoC/XPU/VMIDMT -- to
 * deny, and this machine does not model that fabric yet, so letting QSEE run
 * ends in its panic.  Until it is modelled the policy is to treat the TZ round
 * trip as "returned OK": resume at the APPSBL entry the same image would have
 * started, with r0 = 0.
 *
 * The breakpoint is BP_MACHINE, i.e. it lives in QEMU's breakpoint list and is
 * serviced by the hook below, not by a debugger: nothing is patched in the
 * guest, no host process has to stay attached, and because it is not guest
 * memory it survives SBL1 reloading QSEE on every boot -- including a guest
 * reboot, where the machine starts again at SBL1 and the same redirect has to
 * happen with no harness involvement.
 *
 * QEMU_IPQ_QSEE=run leaves QSEE alone (Step 2 of the bring-up: model the
 * fabric and delete this policy).
 */
static bool ipq_qsee_redirect(CPUState *cs, CPUBreakpoint *bp)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    if (!ipq->appsbl_stage) {
        return false;
    }
    ipq_qsee_skips++;
    fprintf(stderr, "ipq4019: qsee: skip %s@0x%08x -> %s@0x%08x "
            "(lr=0x%08x, skip #%u)\n",
            ipq->qsee_stage->name, ipq->qsee_stage->entry,
            ipq->appsbl_stage->name, ipq->appsbl_stage->entry,
            env->regs[14], ipq_qsee_skips);

    env->regs[15] = ipq->appsbl_stage->entry;
    env->regs[0] = 0;
    /*
     * APPSBL is ARM code and SBL1 called QSEE from Thumb state, so the mode
     * has to be switched back explicitly -- the same thing the harness used
     * to do with `$cpsr &= ~0x20`.
     */
    env->thumb = 0;
    env->uncached_cpsr &= ~CPSR_T;
    return true;
}

static void ipq4019_arm_qsee_policy(void)
{
    const char *policy = getenv("QEMU_IPQ_QSEE");
    CPUState *cs;

    if (!ipq->qsee_stage || !ipq->appsbl_stage) {
        return;
    }
    if (policy && (strcmp(policy, "run") == 0 || strcmp(policy, "0") == 0)) {
        fprintf(stderr, "ipq4019: qsee: QEMU_IPQ_QSEE=%s, leaving %s@0x%08x "
                "to run\n", policy, ipq->qsee_stage->name,
                ipq->qsee_stage->entry);
        return;
    }
    cpu_breakpoint_hook = ipq_qsee_redirect;
    for (cs = first_cpu; cs; cs = CPU_NEXT(cs)) {
        cpu_breakpoint_insert(cs, ipq->qsee_stage->entry, BP_MACHINE, NULL);
    }
    fprintf(stderr, "ipq4019: qsee: %s@0x%08x needs an XPU this machine does "
            "not model; will resume at %s@0x%08x (QEMU_IPQ_QSEE=run keeps "
            "it)\n", ipq->qsee_stage->name, ipq->qsee_stage->entry,
            ipq->appsbl_stage->name, ipq->appsbl_stage->entry);
}

/*
 * The boot chain as the configured boot device presents it.
 *
 * A NOR die is directly addressable, so the image is the die.  A NAND is not:
 * every 2048-byte page carries 64 spare bytes the boot loader drops, so the
 * logical image is the page data with the spare area removed -- exactly what
 * the ROM's NAND path would hand to SBL1.
 */
static gchar *ipq_boot_image(gsize *lenp, bool *owned, const char **what)
{
    IPQNand *n = ipq_board->nand;
    IPQMem *nor = ipq_board->nor;
    gchar *buf;
    uint32_t p;

    switch (ipq_cfg.boot) {
    case IPQ_BOOT_SPI_NOR:
        *lenp = ipq_flash_die_size;
        *owned = false;
        *what = "the NOR die";
        return (gchar *)nor->data;
    case IPQ_BOOT_SPI_NAND:
        buf = g_malloc((gsize)n->pages * NAND_PAGE);
        for (p = 0; p < n->pages; p++) {
            memcpy(buf + (gsize)p * NAND_PAGE, n->data + (gsize)p * NAND_STRIDE,
                   NAND_PAGE);
        }
        *lenp = (gsize)n->pages * NAND_PAGE;
        *owned = true;
        *what = "the NAND die";
        return buf;
    case IPQ_BOOT_QPIC_NAND: {
        IPQParNand *d = ipq_board->qpic ? ipq_board->qpic->die : NULL;

        if (!d) {
            return NULL;
        }
        buf = g_malloc((gsize)d->pages * NAND_PAGE);
        for (p = 0; p < d->pages; p++) {
            memcpy(buf + (gsize)p * NAND_PAGE, d->data + (gsize)p * NAND_STRIDE,
                   NAND_PAGE);
        }
        *lenp = (gsize)d->pages * NAND_PAGE;
        *owned = true;
        *what = "the QPIC NAND die";
        return buf;
    }
    default:
        return NULL;
    }
}

static bool ipq4019_load_elf(MachineState *machine, MemoryRegion *sysmem)
{
    const char *what = NULL;
    bool owned = true;
    gchar *buf;
    gsize len;
    Elf32_Ehdr *eh;
    int i;

    buf = ipq_boot_image(&len, &owned, &what);
    if (!buf) {
        /* No boot device: nothing to boot.  The board config already said so. */
        return false;
    }
    if (len < sizeof(Elf32_Ehdr) || memcmp(buf, ELFMAG, SELFMAG) != 0 ||
        buf[EI_CLASS] != ELFCLASS32 || buf[EI_DATA] != ELFDATA2LSB) {
        if (owned) {
            g_free(buf);
        }
        return false;
    }
    eh = (Elf32_Ehdr *)buf;
    if (eh->e_machine != EM_ARM ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len) {
        if (owned) {
            g_free(buf);
        }
        return false;
    }

    for (i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(buf + eh->e_phoff +
                                        (size_t)i * eh->e_phentsize);
        hwaddr base, end;
        char *name;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) {
            continue;
        }
        base = ph->p_paddr & ~(hwaddr)0xfff;
        end = ((hwaddr)ph->p_paddr + ph->p_memsz + 0xfff) & ~(hwaddr)0xfff;
        if (ipq_range_covered(base, end - base)) {
            continue;
        }
        name = g_strdup_printf("ipq4019.sbl%d", i);
        ipq_map_ram(sysmem, base, end - base, name);
        g_free(name);
    }

    for (i = 0; i < eh->e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(buf + eh->e_phoff +
                                        (size_t)i * eh->e_phentsize);
        char *name;

        if (ph->p_type != PT_LOAD || ph->p_filesz == 0) {
            continue;
        }
        name = g_strdup_printf("sbl.seg%d", i);
        rom_add_blob_fixed(name, buf + ph->p_offset, ph->p_filesz,
                           ph->p_paddr);
        g_free(name);
    }

    ipq4019_elf_entry = eh->e_entry;
    fprintf(stderr, "ipq4019: loaded %s (SBL ELF), entry 0x%08x, %d phdrs\n",
            what, ipq4019_elf_entry, eh->e_phnum);

    /*
     * The same buffer holds the rest of the boot chain (QSEE and the APPSBL),
     * so read their entry points from the images themselves now that the
     * buffer is known to be a boot image.
     */
    ipq4019_scan_stages((const uint8_t *)buf, len);
    if (owned) {
        g_free(buf);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Whole-machine state dump (QEMU_IPQ_STATE_DUMP=<path>)               */
/* ------------------------------------------------------------------ */

/*
 * A warm boot's failure is invisible in the guest's own log: the booter
 * prints the same lines, hands the same image to the same stub, and the stub
 * is the same code.  What can still differ is any register the booter never
 * rewrites -- and the only way to find those without guessing is to write
 * the model's whole register state down at the handoff and diff a cold run
 * against a warm one.
 *
 * Only nonzero registers are printed, so the diff is the leftover state: a
 * block that appears with a value in the warm dump but not in the cold one
 * is exactly what a system reset failed to clear.
 */
static void state_regs(FILE *f, const char *blk, const uint32_t *regs,
                       unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (regs[i]) {
            fprintf(f, "%-14s [%05x] %08x\n", blk, i * 4, regs[i]);
        }
    }
}

/* Same, but only the entries that differ from the block's reset value. */
static void state_regs_diff(FILE *f, const char *blk, const uint32_t *regs,
                            unsigned n, uint32_t def)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (regs[i] != def) {
            fprintf(f, "%-14s [%05x] %08x\n", blk, i * 4, regs[i]);
        }
    }
}

/* 16-bit register banks (a PHY's clause 22 register file). */
static void state_regs16(FILE *f, const char *blk, const uint16_t *regs,
                         unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (regs[i]) {
            fprintf(f, "%-14s [%04x] %04x\n", blk, i * 2, regs[i]);
        }
    }
}

void ipq_state_dump(const char *tag)
{
    const char *path = getenv("QEMU_IPQ_STATE_DUMP");
    FILE *f;
    int i;

    if (!path || !*path) {
        return;
    }
    f = fopen(path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "== %s\n", tag);

    if (ipq->qup_spi) {
        IPQQupl *q = ipq->qup_spi;

        state_regs(f, "qup.shadow", q->shadow, 0x1000 / 4);
        fprintf(f, "qup.slave      phase=%d op=%02x addr=%06x al=%d dl=%d cs=%d "
                "force=%d owner=%d die=%d tgt=%d nrd=%d rd=%d rxoff=%06x "
                "rxlen=%d rxpos=%d wrap=%d sr1=%02x sr2=%02x wsr=%d "
                "pgm=%06x pgl=%d wip=%d wr=%d db=%u last=%06x a4=%d\n",
                q->phase, q->opcode, q->addr, q->addr_left, q->dummy_left,
                q->cs, q->cs_force, q->cs_owner, q->cs_gpio_die, q->target,
                q->nand_read, q->read_data, q->rx_off, q->rx_len, q->rx_pos,
                q->rx_wrap, q->status, q->status2, q->write_sr, q->pgm_addr,
                q->pgm_left, q->wip_pending, q->wr_kind, q->data_bytes,
                q->last_addr, q->addr4);
        fprintf(f, "qup.engine     state=%x opmask=%x txb=%u rxb=%u clk=%u "
                "mid=%x mod=%x\n", q->state, q->op_mask, q->tx_bytes,
                q->rx_bytes, q->clocks, q->max_in_done, q->max_out_done);
    }
    if (ipq->blsp1_bam) {
        IPQBam *b = ipq->blsp1_bam;

        state_regs(f, "bam.shadow", b->shadow, b->nregs);
        fprintf(f, "bam.mask       %08x\n", b->irq_srcs_msk);
        for (i = 0; i < BAM_PIPE_COUNT; i++) {
            IPQBamPipe *p = &b->pipe[i];

            if (p->ctrl || p->event || p->irq_stts || p->irq_en ||
                p->limit || p->ring_base || p->ring_size || p->prod ||
                p->cons || p->rx_desc_open) {
                fprintf(f, "bam.pipe%-2d     ctrl=%x ev=%x sts=%x en=%x "
                        "lim=%x base=%08x size=%x prod=%x cons=%x open=%d\n",
                        i, p->ctrl, p->event, p->irq_stts, p->irq_en,
                        p->limit, p->ring_base, p->ring_size, p->prod,
                        p->cons, p->rx_desc_open);
            }
        }
    }
    if (ipq->console_uart) {
        IPQUart *u = ipq->console_uart;

        fprintf(f, "uart           mr1=%08x mr2=%08x imr=%08x cr=%x tfwr=%x "
                "rfwr=%x dmrx=%x dmen=%x ncf=%x rxhead=%u rxcount=%u "
                "rxqcount=%u rxtotal=%u dma=%d stale=%d\n",
                u->mr1, u->mr2, u->imr, u->cr, u->tfwr, u->rfwr, u->dmrx,
                u->dmen, u->ncf_tx, u->rx_head, u->rx_count, u->rxq_count,
                u->rx_total, u->rx_dma_seen, u->rx_stale);
    }
    if (ipq->edma) {
        IPQEdma *e = ipq->edma;

        state_regs(f, "edma.regs", e->regs, e->nregs);
        for (i = 0; i < IPQ_EDMA_QUEUES; i++) {
            if (e->tx_pkt[i] || e->tx_byte[i] || e->tx_level[i]) {
                fprintf(f, "edma.q%-2d        pkt=%08x byte=%08x lvl=%d\n",
                        i, e->tx_pkt[i], e->tx_byte[i], e->tx_level[i]);
            }
        }
    }
    if (ipq->mdio) {
        IPQMdio *m = ipq->mdio;

        fprintf(f, "mdio           mode=%08x addr=%08x wdata=%08x "
                "rdata=%08x cmd=%x\n", m->mode, m->addr, m->wdata, m->rdata,
                m->cmd);
        for (i = 0; i < m->nphy; i++) {
            state_regs16(f, "mdio.phy", m->phy[i].reg, 32);
        }
    }
    if (ipq->tlmm) {
        state_regs_diff(f, "tlmm", ipq->tlmm->regs, ipq->tlmm->nregs,
                        IPQ_TLMM_RESET_VAL);
    }
    if (ipq->regfile_list) {
        for (i = 0; i < (int)ipq->regfile_list->len; i++) {
            IPQRegFile *r = g_ptr_array_index(ipq->regfile_list, i);

            state_regs(f, memory_region_name(&r->mr), r->regs, r->nregs);
        }
    }
    if (ipq->stub_list) {
        for (i = 0; i < (int)ipq->stub_list->len; i++) {
            IPQStub *st = g_ptr_array_index(ipq->stub_list, i);

            if (st->nregs) {
                state_regs(f, st->name, st->shadow, st->nregs);
            }
        }
    }
    if (ipq_board->nand) {
        IPQNand *n = ipq_board->nand;

        fprintf(f, "nand           st=%02x cfg=%02x lock=%02x oip=%d ecc=%d "
                "page=%u col=%u cache_valid=%d wr=%d feat=%02x\n",
                n->status, n->cfg, n->lock, n->oip, n->ecc, n->page, n->col,
                n->cache_valid, n->wr_kind, n->wr_feature);
    }
    if (ipq->wdt) {
        state_regs(f, "wdt", ipq->wdt->regs, 0x1000 / 4);
    }
    if (ipq->pshold) {
        fprintf(f, "pshold         val=%x\n", ipq->pshold->val);
    }
    if (current_cpu) {
        CPUARMState *e = &ARM_CPU(current_cpu)->env;

        fprintf(f, "cpu            cpsr=%08x sctlr=%08" PRIx64
                " ttbr0=%08" PRIx64 " vbar=%08" PRIx64
                " cntfrq=%08" PRIx64 " pmcr=%08" PRIx64 "\n",
                e->uncached_cpsr, e->cp15.sctlr_el[1], e->cp15.ttbr0_el[1],
                e->cp15.vbar_el[1], e->cp15.c14_cntfrq, e->cp15.c9_pmcr);
        for (i = 0; i < 16; i++) {
            fprintf(f, "cpu.r%-2d        %08x\n", i, e->regs[i]);
        }
    }
    fprintf(f, "--\n");
    fclose(f);
}

/* Watch the console bytes for the booter's handoff line. */
void state_note_tx(const uint8_t *buf, unsigned n)
{
    static int on = -1;
    static char tail[32];
    static unsigned tlen;

    if (on < 0) {
        const char *v = getenv("QEMU_IPQ_STATE_DUMP");

        on = v && *v;
    }
    if (!on) {
        return;
    }
    while (n--) {
        uint8_t c = *buf++;

        if (tlen == sizeof(tail) - 1) {
            memmove(tail, tail + 1, sizeof(tail) - 2);
            tlen--;
        }
        tail[tlen++] = c;
        tail[tlen] = 0;
        if (strstr(tail, "Jumping to kernel")) {
            tlen = 0;
            tail[0] = 0;
            ipq_state_dump("handoff");
        }
    }
}

/*
 * A system reset drops the die's controller state, not its array.
 *
 * The page cache, the latched row/column, the busy and write-enable bits and
 * the half-finished write kind all belong to the controller: after power-on the
 * part answers "not busy, cache invalid" and the boot ROM latches its own read
 * from scratch.  Leaving Linux's last latched page and cache behind makes the
 * next booter's READ FROM CACHE hand the stub whatever the previous OS last
 * read, instead of the page it just asked for.  n->data (the array) and its
 * backing file of course survive -- that is the point of a reboot.
 */
static void nand_dev_reset(IPQNand *n)
{
    n->status = 0;
    n->cfg = 0;
    n->lock = 0;
    n->oip = 0;
    n->ecc = 0;
    n->page = 0;
    n->col = 0;
    nand_fill_erased(n->cache, NAND_STRIDE);
    n->cache_valid = 0;
    n->wr_kind = NAND_WR_NONE;
    n->wr_feature = 0;
}

/*
 * Power-on reset of the modelled hardware.
 *
 * A system reset drops the SoC's state, and the boot chain depends on it: the
 * second SBL1 run re-probes the SPI controller and the BAM assuming they are
 * in their reset state, so leaving Linux's leftovers in the QUP's FIFOs makes
 * boot_flash_init fail with a die that is perfectly fine.  The pin controller
 * and the NAND controller's page cache are the same story: a warm boot has to
 * start from their reset values even though the flash array keeps what the
 * previous OS wrote.  The one thing that must survive is the flash array
 * itself -- that is the point of a reboot.
 */
/*
 * Every block that has state to drop owns a reset function, and the table
 * below is the complete list of them.
 *
 * The point is not style.  Each of the leftovers found on this machine so far
 * -- the pin controller still muxed by Linux, the NAND controller's latched
 * page cache, the ESS register file, the level-high QUP/BAM/EDMA interrupt
 * lines -- was a block that simply was not in the reset sequence.  A
 * hand-written sequence cannot show that omission; a table can, and adding a
 * device without adding its entry is visible in one place.
 *
 * What must *not* be here: the NOR die, the NAND array and their persist
 * files.  Those are the machine's non-volatile storage, and surviving a reset
 * is the entire point of a reboot.  Their controller state is reset (QUP for
 * the NOR, nand_dev_reset() for the NAND) -- the arrays are not.
 *
 * QEMU_IPQ_NO_RESET_<NAME> skips one entry (same "0 means off" rule as every
 * other knob); QEMU_IPQ_NO_DEV_RESET skips the whole table.
 * QEMU_IPQ_RESET_LOG=1 prints which blocks ran.
 */

static void reset_counter(void)
{
    /*
     * Start the QTIMER/GCNT/XO/TSC frames from zero again (SBL1's log stamps,
     * udelay() baselines and the kernel's early delays all read them).
     */
    ipq_counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void reset_qup(void)
{
    IPQQupl *q = ipq->qup_spi;
    uint8_t *resp;
    uint32_t resp_cap;
    qemu_irq irq;

    if (!q) {
        return;
    }
    resp = q->resp;
    resp_cap = q->resp_cap;
    irq = q->irq;
    memset((char *)q + offsetof(IPQQupl, phase), 0,
           sizeof(*q) - offsetof(IPQQupl, phase));
    q->resp = resp;
    q->resp_cap = resp_cap;
    q->irq = irq;
    memset(q->shadow, 0, sizeof(q->shadow));
    /*
     * The interrupt is level-high and the GIC keeps whatever level the line
     * had.  Zeroing the register file does not lower it, so after a booted
     * Linux the next boot would start with SPI 95 already asserted -- an
     * interrupt with no cause.  Power-on drops the line.
     */
    qemu_set_irq(q->irq, 0);
}

static void reset_bam(void)
{
    IPQBam *b = ipq->blsp1_bam;
    int i;

    if (!b) {
        return;
    }
    memset(b->shadow, 0, b->nregs * sizeof(uint32_t));
    memset(b->valid, 0, b->nregs);
    /*
     * BAM_IRQ_SRCS_MSK is its own field, not part of the shadow register file,
     * so clearing the file left Linux's mask behind: power-on reads 0, and a
     * warm boot read 0x80000033.  The mask gates bam_update_irq(), so a stale
     * one re-asserts SPI 238 from sources the next boot believes are masked.
     */
    b->irq_srcs_msk = 0;
    for (i = 0; i < BAM_PIPE_COUNT; i++) {
        BAMClient client = b->pipe[i].client;

        memset(&b->pipe[i], 0, sizeof(b->pipe[i]));
        b->pipe[i].client = client;
    }
    bam_seed_id(b);
    qemu_set_irq(b->irq, 0);
}

static void reset_uart(void)
{
    IPQUart *u = ipq->console_uart;

    if (!u) {
        return;
    }
    timer_del(u->stale_timer);
    u->mr1 = u->mr2 = u->imr = u->cr = 0;
    u->tfwr = u->rfwr = u->dmrx = u->dmen = u->ncf_tx = 0;
    u->rx_head = u->rx_count = 0;
    u->rxq_head = u->rxq_count = 0;
    u->rx_total = 0;
    u->rx_stale = false;
    u->rx_dma_seen = false;
    uart_update_irq(u);
}

static void reset_mdio(void)
{
    int i;

    if (!ipq->mdio) {
        return;
    }
    ipq->mdio->mode = ipq->mdio->addr = ipq->mdio->wdata = 0;
    ipq->mdio->rdata = ipq->mdio->cmd = 0;
    for (i = 0; i < ipq->mdio->nphy; i++) {
        phy_reset_regs(&ipq->mdio->phy[i]);
    }
}

static void reset_edma(void)
{
    int i;

    if (!ipq->edma) {
        return;
    }
    memset(ipq->edma->regs, 0, ipq->edma->nregs * sizeof(uint32_t));
    memset(ipq->edma->tx_pkt, 0, sizeof(ipq->edma->tx_pkt));
    memset(ipq->edma->tx_byte, 0, sizeof(ipq->edma->tx_byte));
    memset(ipq->edma->tx_level, 0, sizeof(ipq->edma->tx_level));
    for (i = 0; i < IPQ_EDMA_QUEUES; i++) {
        qemu_set_irq(ipq->edma->irq_tx[i], 0);
        qemu_set_irq(ipq->edma->irq_rx[i], 0);
    }
}

static void reset_stubs(void)
{
    int i;

    for (i = 0; ipq->stub_list && i < (int)ipq->stub_list->len; i++) {
        IPQStub *st = g_ptr_array_index(ipq->stub_list, i);

        if (st->nregs) {
            memset(st->shadow, 0, st->nregs * sizeof(uint32_t));
            memset(st->valid, 0, st->nregs);
        }
        st->spin = 0;
    }
}

static void reset_tlmm(void)
{
    if (ipq->tlmm) {
        tlmm_dev_reset(ipq->tlmm);
    }
}

static void reset_nand(void)
{
    if (ipq_board->nand) {
        nand_dev_reset(ipq_board->nand);
    }
}

static void reset_qpic(void)
{
    qpic_dev_reset();
}

static void reset_regfiles(void)
{
    int i;

    if (!ipq->regfile_list) {
        return;
    }
    for (i = 0; i < (int)ipq->regfile_list->len; i++) {
        IPQRegFile *r = g_ptr_array_index(ipq->regfile_list, i);

        if (r->por) {
            memcpy(r->regs, r->por, r->nregs * sizeof(uint32_t));
        } else {
            memset(r->regs, 0, r->nregs * sizeof(uint32_t));
        }
    }
}

/* Freeze every register file at its power-on contents; call once, at init. */
static void regfile_seal_all(void)
{
    int i;

    if (!ipq->regfile_list) {
        return;
    }
    for (i = 0; i < (int)ipq->regfile_list->len; i++) {
        IPQRegFile *r = g_ptr_array_index(ipq->regfile_list, i);

        r->por = g_renew(uint32_t, r->por, r->nregs);
        memcpy(r->por, r->regs, r->nregs * sizeof(uint32_t));
    }
}

static void reset_pshold(void)
{
    if (ipq->pshold) {
        ipq->pshold->val = 1;            /* rails back up */
    }
}

static void reset_wdt(void)
{
    if (ipq->wdt) {
        wdt_disarm(ipq->wdt);
        memset(ipq->wdt->regs, 0, sizeof(ipq->wdt->regs));
    }
}

typedef struct IPQResetEntry {
    const char *name;
    void (*fn)(void);
} IPQResetEntry;

static const IPQResetEntry ipq_reset_table[] = {
    { "COUNTER",  reset_counter  },
    { "TLMM",     reset_tlmm     },
    { "QUP",      reset_qup      },
    { "NAND",     reset_nand     },
    { "QPIC",     reset_qpic     },
    { "BAM",      reset_bam      },
    { "UART",     reset_uart     },
    { "MDIO",     reset_mdio     },
    { "EDMA",     reset_edma     },
    { "ESS",      reset_regfiles },
    { "STUBS",    reset_stubs    },
    { "PSHOLD",   reset_pshold   },
    { "WDT",      reset_wdt      },
};

static void ipq4019_dev_reset(void *opaque)
{
    unsigned i;

    /* What the previous OS left behind, if a dump is configured. */
    if (ipq_counter_epoch_ns) {
        ipq_state_dump("reset");
    }

    if (env_true("QEMU_IPQ_NO_DEV_RESET")) {
        return;
    }

    for (i = 0; i < ARRAY_SIZE(ipq_reset_table); i++) {
        char knob[64];

        snprintf(knob, sizeof(knob), "QEMU_IPQ_NO_RESET_%s",
                 ipq_reset_table[i].name);
        if (env_true(knob)) {
            continue;
        }
        if (env_true("QEMU_IPQ_RESET_LOG")) {
            fprintf(stderr, "reset: %s\n", ipq_reset_table[i].name);
        }
        ipq_reset_table[i].fn();
    }

    /*
     * What the machine starts the next boot with.  Comparing this snapshot
     * across a reboot is how the reset is *shown* to be complete instead of
     * assumed: a block that appears in the warm snapshot but not in the cold
     * one is state the table above still misses.
     */
    ipq_state_dump("postreset");
}

static void ipq4019_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint32_t entry = ipq4019_elf_entry ?: env_u32("QEMU_IPQ_ENTRY", 0);

    /*
     * Reset the CPU itself first.
     *
     * CPU objects sit on no bus, so nothing resets them during a system reset
     * unless somebody asks: arm_load_kernel() registers do_cpu_reset() (which
     * calls cpu_reset()) only on the path that loads a kernel, and this board
     * places the SBL1 ELF itself instead.  Without this, a guest reboot or a
     * monitor system_reset left SCTLR/VBAR/CNTVOFF exactly as the kernel had
     * them, so the CPU began executing SBL1 with the MMU on and the old
     * exception vectors live -- which shows up as a kernel oops at the SBL1
     * entry point instead of a fresh boot log.
     */
    cpu_reset(CPU(cpu));

    /*
     * Reset the instruction cache, i.e. QEMU's translated blocks.
     *
     * This is not a speed tweak: a real CPU reset invalidates the I-cache, so
     * nothing the previous OS translated may be executed after a reboot.  Here
     * the translated blocks are that cache -- CPU objects sit on no bus and a
     * system reset only flushes them when the code buffer overflows -- so
     * without this the second boot re-enters the booter through blocks the
     * booted Linux had already translated, while a cold boot has none.
     *
     * It is safe to queue from the reset handler: the flush runs at the next
     * safe point, before any vCPU executes guest code again.
     */
    if (!env_true("QEMU_IPQ_NO_TB_FLUSH")) {
        queue_tb_flush(CPU(cpu));
    }

    /*
     * The IPQ40xx boot ROM leaves the PMU cycle counter running; RouterBOOT's
     * delay loops read PMCCNTR (CP15 c9/c14). QEMU resets PMCR.E to 0, so
     * restore what the ROM would have left behind.
     */
    if (!env_true("QEMU_IPQ_NO_PMU_FIX")) {
        env->cp15.c9_pmcr |= 1;          /* PMCR.E */
        env->cp15.c9_pmcnten |= 1u << 31; /* PMCNTENSET.C, counter 31 */
        env->cp15.c9_pmuserenr |= 1;     /* PMUSERENR.EN */
    }

    if (ipq4019_bootinfo) {
        env->regs[0] = ipq4019_bootinfo;
    }

    if (entry) {
        env->regs[15] = entry;
        env->thumb = 0;
    }
}

/*
 * Debug: print the architectural timer state once a second while the guest
 * runs (QEMU_IPQ_TIMER_DBG=1).  Useful when a kernel boots but never takes a
 * tick: it shows whether the timer was programmed, whether ISTATUS is set, and
 * when QEMU intends to expire it.
 */

static void ipq4019_timer_dbg(void *opaque)
{
    ARMCPU *cpu = ARM_CPU(first_cpu);
    CPUARMState *env = &cpu->env;
    static const char *const names[] = { "phys", "virt", "hyp", "sec" };
    int i;

    fprintf(stderr, "ipq4019-timer: cntfrq=%" PRIu64 " cntvoff=%" PRIu64 " cnthctl=%" PRIx64
           " daif=%02x irq_line=%x\n", cpu->gt_cntfrq_hz,
           env->cp15.cntvoff_el2, env->cp15.cnthctl_el2,
           (unsigned)env->daif, env->irq_line_state);
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        ARMGenericTimer *gt = &env->cp15.c14_timer[i];

        fprintf(stderr, "  %s ctl=%03" PRIx64 " cval=%" PRIu64 " expire_ns=%" PRId64
               "\n", names[i], gt->ctl, gt->cval,
               timer_expire_time_ns(cpu->gt_timer[i]) -
               qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
    fflush(stdout);
    timer_mod_ns(ipq->timer_dbg,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
}

/*
 * Debug: host-clock heartbeat (QEMU_IPQ_DIAG=1).  Runs on QEMU_CLOCK_HOST so
 * that it keeps printing even if the virtual clock has stopped, which is the
 * only way to tell "guest spins" from "guest waits for a timer forever".
 */
static int64_t ipq_diag_last_virtual;

static void ipq4019_diag(void *opaque)
{
    CPUState *cs = first_cpu;
    ARMCPU *cpu = ARM_CPU(cs);
    int64_t v = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t h = qemu_clock_get_ns(QEMU_CLOCK_HOST);

    fprintf(stderr, "diag: state=%s h=%" PRId64 " v=%" PRId64 " dv=%" PRId64
           " pc=%08x halted=%d irqreq=%x daif=%02x\n",
           RunState_str(runstate_get()),
           h, v, v - ipq_diag_last_virtual,
           (uint32_t)cpu->env.regs[15], cs->halted,
           cs->interrupt_request, (unsigned)cpu->env.daif);
    if (ipq->edma) {
        fprintf(stderr, "diag: net tx=%" PRIu64 " (noport=%" PRIu64 " nolink=%" PRIu64
               ") rx=%" PRIu64 " (dropped=%" PRIu64 ")\n",
               ipq->edma->tx_host, ipq->edma->tx_drop_noport,
               ipq->edma->tx_drop_nolink, ipq->edma->rx_host,
               ipq->edma->rx_dropped);
    }
    fflush(stdout);
    ipq_diag_last_virtual = v;
    timer_mod_ns(ipq->diag_timer, h + 2 * NANOSECONDS_PER_SECOND);
}

/*
 * Which die boots, and what the command line offered to program it with.  The
 * boot chain always comes from the die, so "the image" here is a programmer:
 * it is either what filled the die on this run or the thing that was ignored
 * because the die already held one.
 */
static void ipq_boot_dev_info(const char **dev, const char **img,
                              const char **store, bool *loaded)
{
    *dev = ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "NOR" :
           ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "NAND" :
           ipq_cfg.boot == IPQ_BOOT_QPIC_NAND ? "QPIC NAND" : NULL;
    *img = ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? ipq_cfg.nor_image :
           ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? ipq_cfg.nand_image :
           ipq_cfg.boot == IPQ_BOOT_QPIC_NAND ? ipq_cfg.qpic_nand_image : NULL;
    *store = ipq_cfg.boot == IPQ_BOOT_SPI_NOR
           ? (ipq_cfg.nor_file ? ipq_cfg.nor_file : ipq_cfg.nor_drive)
           : ipq_cfg.boot == IPQ_BOOT_SPI_NAND
           ? (ipq_cfg.nand_file ? ipq_cfg.nand_file : ipq_cfg.nand_drive)
           : (ipq_cfg.qpic_nand_file ? ipq_cfg.qpic_nand_file
                                     : ipq_cfg.qpic_nand_drive);
    *loaded = ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? ipq_nor_store_loaded
            : ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? ipq_nand_store_loaded
            : ipq_qpic_store_loaded;
}

static void ipq4019_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rom;
    IPQMem *bootctx;
    IPQMem *bootdev;
    IPQBam *bam;
    MemoryRegion *gcnt;
    MemoryRegion *qtimer;
    MemoryRegion *xo_timer;
    MemoryRegion *tsc_timer;
    uint32_t entry = env_u32("QEMU_IPQ_ENTRY", 0);
    int i;

    ipq = g_new0(IPQ4019State, 1);
    ipq_board = g_new0(IPQBoardState, 1);
    ipq_board_config_resolve(machine);

    for (i = 0; i < machine->smp.cpus; i++) {
        cpu_create(machine->cpu_type);
    }
    if (ipq_cfg.poweroff || env_true("QEMU_IPQ_HANG_LOG")) {
        ipq_poweroff_start();
    }

    if (env_true("QEMU_IPQ_DIAG")) {
        ipq_diag_last_virtual = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ipq->diag_timer = timer_new_ns(QEMU_CLOCK_HOST, ipq4019_diag, NULL);
        timer_mod_ns(ipq->diag_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_HOST) +
                     2 * NANOSECONDS_PER_SECOND);
    }

    if (env_true("QEMU_IPQ_TIMER_DBG")) {
        ipq->timer_dbg = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipq4019_timer_dbg,
                                     NULL);
        timer_mod_ns(ipq->timer_dbg, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     NANOSECONDS_PER_SECOND);
    }

    /*
     * SMC conduit.
     *
     * On real ipq40xx hardware the QSEE/TZ monitor (loaded into 0x87e80000)
     * owns EL3 and services every smc from SBL1, RouterBOOT and Linux.  This
     * machine does not model the protection fabric QSEE needs, so nothing
     * installs a monitor: HVBAR stays 0 and an smc from SVC takes the
     * exception vector at 0x8, which is how the OpenWrt kernel dies with
     * "Bad mode in prefetch abort handler" at pc == 0x8 in MON mode.
     *
     * QEMU has an EL3 stand-in for exactly this situation: with
     * cpu->psci_conduit = SMC, target/arm/tcg/psci.c consumes the smc and
     * answers unknown function IDs with PSCI_RET_NOT_SUPPORTED instead of
     * trapping.  Linux's qcom_scm client then sees a clean error return, and
     * drivers that need the monitor fail to probe instead of taking the
     * whole kernel down.
     *
     * Set it directly on the CPUs rather than through arm_load_kernel():
     * this board boots raw firmware (info->is_linux == 0, boot_el == 3 for
     * cortex-a7), and arm_boot.c deliberately disables the SMC conduit in
     * that case.  The property is documented as settable after realize.
     *
     * QEMU_IPQ_PSCI: "smc" (default), "hvc" or "none".
     */
    {
        const char *psci = getenv("QEMU_IPQ_PSCI");
        uint32_t conduit = QEMU_PSCI_CONDUIT_SMC;

        if (psci && !strcmp(psci, "none")) {
            conduit = QEMU_PSCI_CONDUIT_DISABLED;
        } else if (psci && !strcmp(psci, "hvc")) {
            conduit = QEMU_PSCI_CONDUIT_HVC;
        } else if (psci && strcmp(psci, "smc")) {
            error_report("ipq4019: QEMU_IPQ_PSCI=%s must be smc, hvc or none",
                         psci);
            exit(1);
        }
        for (CPUState *cs = first_cpu; cs; cs = CPU_NEXT(cs)) {
            object_property_set_int(OBJECT(cs), "psci-conduit",
                                    conduit, &error_abort);
        }
        fprintf(stderr, "ipq4019: psci conduit %s\n",
               conduit == QEMU_PSCI_CONDUIT_SMC ? "smc" :
               conduit == QEMU_PSCI_CONDUIT_HVC ? "hvc" : "disabled");
    }

    memory_region_add_subregion(sysmem, IPQ_RAM_BASE, machine->ram);
    ipq->dram = machine->ram;
    ipq_dram_size = machine->ram_size;
    qemu_register_reset(ipq_reset_dump, NULL);

    ipq_board_add_nor(sysmem);
    /* The second die on the bus: a blank SPI NAND behind chip select 1.  It is
     * opt-in (QEMU_IPQ_NAND=1 or a NAND die file) because the boards whose
     * dumps are here have only the NOR, and a part nobody probes still costs
     * 132 MiB of RAM. */
    ipq_board_add_nand();
    /*
     * The parallel NAND, when the board has one.  It is a separate part on a
     * separate bus (QPIC + its own BAM), so it does not share anything with
     * the SPI die above but the die format.
     */
    ipq_board_add_qpic_nand(sysmem);
    /*
     * MikroTik's APPSBL ELF header is written with a file-offset-compensated
     * p_paddr: the segment starts at 0x7fffffac so that file offset 0x54 (the
     * first byte after the ELF header) lands exactly on the entry point
     * 0x80000000.  SBL1's loader copies from p_paddr, so it writes the 0x54
     * header bytes into the page just below DDR.  Give it scratch memory there
     * instead of letting the write take a data abort.
     */
    ipq_map_ram(sysmem, 0x7ffff000, 0x1000, "ipq4019.appsbl-elfpad");

    rom = g_new0(MemoryRegion, 1);
    memory_region_init_ram(rom, NULL, "ipq4019.rom", IPQ_ROM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, IPQ_ROM_BASE, rom);
    if (machine->firmware) {
        if (load_image_targphys(machine->firmware, IPQ_ROM_BASE, IPQ_ROM_SIZE,
                                NULL) < 0) {
            error_report("could not load '%s' into boot ROM", machine->firmware);
            exit(1);
        }
    } else if (!machine->kernel_filename) {
        warn_report("no -bios: boot ROM at 0x0 is empty");
    }

    add_stub(sysmem, IPQ_PRNG_BASE, 0x1000, "prng");
    add_pshold(sysmem);
    add_wdt(sysmem);
    add_stub(sysmem, 0x400000, 0x1000, "ddr-ctl");
    add_stub(sysmem, 0x4a0000, 0x1000, "timer-ctl");
    /*
     * SBL1's udelay() polls a 56-bit free running counter at 0x4a2000
     * (low word at +0, high word at +4).  This is the QTIMER frame, clocked
     * at 48 MHz: U-Boot's udelay() scales microseconds by 0x30 (48) when it
     * reads this counter, and its mdelay()/poll loops only terminate if the
     * counter really ticks at 48 MHz.
     */
    qtimer = g_new0(MemoryRegion, 1);
    qtimer_hz = env_u32("QEMU_IPQ_QTIMER_HZ", 48000000);
    memory_region_init_io(qtimer, NULL, &gcnt_ops, &qtimer_hz, "qtimer", 8);
    memory_region_add_subregion(sysmem, 0x4a2000, qtimer);
    add_stub(sysmem, 0x4a3000, 0x8000, "timer-misc");
    add_tlmm(sysmem, IPQ_TLMM_BASE, IPQ_TLMM_SIZE, "tlmm");
    /*
     * The board says which pins drive the dies' chip selects, so that is where
     * the dies are connected: the pin controller is the SoC's, the wiring
     * between it and the parts is the board's.
     */
    if (ipq_cfg.nor) {
        ipq_tlmm_connect(ipq->tlmm, ipq_cfg.nor_cs_gpio, ipq_cs_line,
                         (void *)(uintptr_t)(NAND_CS_NOR |
                                             (ipq_cfg.nor_cs_gpio << 8)));
        /*
         * A board that also wires the NOR select to a plain GPIO (the hAP
         * ac3, see nor_cs_gpio_alt): the same die, a second way to reach it.
         */
        if (ipq_cfg.nor_cs_gpio_alt &&
            ipq_cfg.nor_cs_gpio_alt != ipq_cfg.nor_cs_gpio) {
            ipq_tlmm_connect(ipq->tlmm, ipq_cfg.nor_cs_gpio_alt, ipq_cs_line,
                             (void *)(uintptr_t)(NAND_CS_NOR |
                                             (ipq_cfg.nor_cs_gpio_alt << 8)));
        }
    }
    if (ipq_cfg.nand) {
        ipq_tlmm_connect(ipq->tlmm, ipq_cfg.nand_cs_gpio, ipq_cs_line,
                         (void *)(uintptr_t)(NAND_CS_NAND |
                                             (ipq_cfg.nand_cs_gpio << 8)));
    }
    /* The board's face.  Pins are claimed in order, so a mistake is refused
     * here instead of at the first press. */
    ipq_board_add_leds();
    ipq_board_add_keys();
    /*
     * Clock blocks: bit 0 behaves as a self-clearing trigger on written
     * registers (software sets it, then polls until it reads 0).  Unwritten
     * registers keep reading 0xffffffff, which the PLL lock polls want.
     */
    {
        /*
         * GCC is the one block whose untouched registers must not read ~0:
         * QSEE's clock driver does read-modify-write on clock branch/reset
         * registers, and with ~0 as the read value it writes ~0 back, which
         * then reads as "block held in reset" forever -- e.g. it spins on
         * GCC_PRNG_BCR (0x13000) before touching the PRNG at 0x22100.
         * Reads of unimplemented offsets return 0 and most GCC registers
         * reset to 0, so answer 0.
         */
        IPQStub *gcc = add_stub_ex(sysmem, IPQ_GCC_BASE, IPQ_GCC_SIZE,
                                   "gcc", 0xffffffffu);

        gcc->trig_bit0 = env_u32("QEMU_IPQ_TRIG_BIT0", 1) != 0;
        gcc->cbc_status = env_u32("QEMU_IPQ_CBC_STATUS", 1) != 0;
        gcc->def_read = 0;
        /*
         * The PLLs are already running when SBL1 starts: on silicon the boot
         * ROM programs them before it loads SBL1, and SBL1 only reprograms
         * what it needs.  Starting the machine at SBL1 therefore means seeding
         * that inherited state, otherwise every clock the kernel derives from
         * a PLL reads 0 (and clk_set_rate() of the CPU clock fails -EINVAL,
         * which the kernel turns into a BUG_ON in cpufreq_online()).
         *
         *   FEPLL   0x2f020  2 * fdbkdiv / refclkdiv * XO = 4 GHz VCO
         *                    fdbkdiv 125, refclkdiv 3 -> fepll200 /20 = 200M,
         *                    fepll500 /8 = 500M, fepll125 /32 = 125M
         *   DDRPLLAPSS 0x2e020  same VCO field, plus the divider in bits 7:4:
         *                    VCO 5376 MHz, cdiv 13 -> 5376*2/28 = 384 MHz
         *                    that is the CPU clock SBL1 selects (CFG_RCGR
         *                    0x101: parent DDRPLLAPSS, hid_div 1)
         */
        if (env_u32("QEMU_IPQ_PLL_SEED", 1)) {
            /*
             * Pinned, not seeded: these registers describe what the boot ROM
             * programmed and what the OTP says, so they read the same before
             * and after a warm reset.  A reset handler that cleared them made
             * the second boot's cpufreq probe hit the BUG_ON above, which is
             * exactly the failure this seeding exists to avoid.
             */
            stub_pin(gcc, 0x2f020, 0x037d0000u);
            stub_pin(gcc, 0x2e020, 0x013800d0u);
        }
        /*
         * 0x2e000..0x2e02f is the APSS/DDR PLL divider block
         * (gcc_apss_cpu_plldiv_clk in mainline: enable_reg 0x2e000 bit 0,
         * cdiv at 0x2e020); 0x2f000 is the twin used for the CPU divider.
         * Bit 0 of their +0x0c status word is where the divider reports that
         * it has settled: SBL1's clock_init polls it for up to 7000 us and,
         * when it is already set, treats the divider as "nothing to do".
         * Both PLLs are running out of reset, so report them locked.
         */
        stub_pin(gcc, 0x2e00c, 0x1);
        stub_pin(gcc, 0x2f00c, 0x1);
    }
    /*
     * BLSP1 BAM (dma@7884000, 0x23000): the QUP FIFOs can be driven either by
     * the CPU or by this DMA engine, and QCA's boot chain always uses the
     * engine.  It writes a ring of {addr, ctrl} descriptors into DRAM and
     * rings a per-pipe doorbell; see add_bam() for the register map.
     *
     * Two identification registers matter:
     *
     *   BAM_REVISION  (+0x1000) is {major:8, minor:8, num_ees:4, step:8} in
     *                 QCA's bam.h.  SBL1's flash driver only accepts the step
     *                 field values 6..11 / 19..25 / 32..39, and Linux's
     *                 bam_init() reads NUM_EES from bits 15:8 to range-check
     *                 the "qcom,ee" property -- with a zero there the probe
     *                 fails with -EINVAL and spi-qup defers for ever.
     *   BAM_NUM_PIPES (+0x1008) is the pipe count Linux allocates channels
     *                 for.  The IPQ4019 BAM has twelve.
     */
    bam = add_bam(sysmem, IPQ_BLSP1_BAM_BASE, IPQ_BLSP1_BAM_SIZE, "blsp1-bam");
    bam_wire_pipes(bam);
    ipq->blsp1_bam = bam;
    bam_seed_id(bam);
    add_qupl(sysmem, IPQ_QUP_BASE, 0x1000, "qup-spi");
    add_stub(sysmem, IPQ_QPIC_BASE, IPQ_QPIC_SIZE, "qpic-nand");
    /*
     * APCS clock/reset block: same self-clearing bit-0 trigger as the GCC
     * reset registers, polled by the shared clk_set_rate() helper.  Only the
     * low register page needs shadow storage.
     */
    {
        IPQStub *apcs = add_stub_ex(sysmem, IPQ_QGIC_BASE, 0x20000, "apcs",
                                    0xffffffffu);

        apcs->trig_bit0 = env_u32("QEMU_IPQ_TRIG_BIT0", 1) != 0;
        apcs->cbc_status = env_u32("QEMU_IPQ_CBC_STATUS", 1) != 0;
    }
    /*
     * The rest of the APCS range is a decode-only stub, minus the 0xb021000
     * page which carries the free-running XO counter (see xo-timer below).
     * Carve it out here so the two do not overlap.
     */
    add_stub(sysmem, IPQ_QGIC_BASE + 0x20000, 0x1000, "apcs-hi");
    add_stub(sysmem, IPQ_QGIC_BASE + 0x22000, 0xfe0000 - 0x2000, "apcs-hi2");
    /*
     * QGIC2 interrupt controller.
     *
     * Everything above is a decode-only stub, which is fine for the boot
     * chain (nothing there has interrupts enabled) but useless for Linux: a
     * kernel with no GIC runs until the first msleep(), then idles forever
     * waiting for a timer tick that never arrives.  The board DTS describes
     * "qcom,msm-qgic2" at <0xb000000 0x1000 0xb002000 0x1000>, i.e. a plain
     * GICv2 distributor plus CPU interface, so map QEMU's arm_gic there.
     *
     * These subregions are added after the stubs so they win the overlap.
     *
     * Interrupt wiring follows the DTS: the ipq40xx timer node asks for PPIs
     * 2/3/4/1 (secure-phys, non-secure-phys, virt, hyp) rather than the
     * architectural 13/14/11/10, so the generic timer outputs are routed to
     * those INTIDs.  QEMU_IPQ_GIC=none keeps the old stub-only behaviour.
     */
    {
        const char *gic_opt = getenv("QEMU_IPQ_GIC");

        if (!(gic_opt && !strcmp(gic_opt, "none"))) {
            static const int timer_intid[NUM_GTIMERS] = {
                [GTIMER_PHYS] = 19,     /* PPI 3 */
                [GTIMER_VIRT] = 20,     /* PPI 4 */
                [GTIMER_HYP]  = 17,     /* PPI 1 */
                [GTIMER_SEC]  = 18,     /* PPI 2 */
            };
            DeviceState *gicdev = qdev_new(TYPE_ARM_GIC);
            int n;

            /*
             * The SoC has four Cortex-A7s (cpu@0..cpu@3 in the DTS), and the
             * count here is load-bearing rather than cosmetic: with num-cpu
             * 1 QEMU's arm_gic RAZ/WIs the SGI/PPI target registers, so the
             * guest's gic_get_cpumask() reads 0 and leaves gic_cpu_map[0] =
             * 0.  A guest whose DT lists more than one CPU has nr_cpu_ids !=
             * 1, which makes gic_raise_softirq() take the target-mask path
             * instead of the "only one CPU, do a self-IPI" shortcut, so it
             * writes GICD_SGIR with an empty CPU mask and the IPI is
             * dropped.
             *
             * Nothing else needs an IPI here (the other CPUs never come
             * online -- qcom_scm_set_cold_boot_addr needs the QSEE/TZ
             * bring-up this machine does not model -- so the kernel prints
             * "Failed to set CPU boot address, disabling SMP" and runs one
             * CPU), but irq_work does: irq_work_sync() spins on
             * IRQ_WORK_BUSY until the work runs, and the work only runs from
             * the IPI handler.  RouterOS's cpufreq governor stop does exactly
             * that during /system reboot, so with the IPI dropped the clean
             * shutdown waits forever ("soft lockup ... [init]") and never
             * reaches the pshold write.  The extra CPU interfaces are simply
             * never used.
             */
            qdev_prop_set_uint32(gicdev, "num-cpu", IPQ_GIC_NUM_CPU);
            qdev_prop_set_uint32(gicdev, "num-irq", IPQ_GIC_NUM_IRQ);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(gicdev), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(gicdev), 0, IPQ_QGIC_BASE);
            sysbus_mmio_map(SYS_BUS_DEVICE(gicdev), 1, IPQ_QGIC_BASE + 0x2000);
            /*
             * SPI lines the board uses in the DTS: <GIC_SPI 238> is the BAM
             * and <GIC_SPI 95> the QUP.  arm_gic lays its input lines out as
             * [SPIs][32 PPIs per CPU] and adds GIC_INTERNAL back in
             * gic_set_irq(), so input line n is SPI n.
             */
            bam->irq = qdev_get_gpio_in(gicdev, 238);
            /* QPIC's own BAM: <GIC_SPI 101> (qcom-ipq4019.dtsi, dma@7984000) */
            if (ipq_qpic_bam) {
                ipq_qpic_bam->irq = qdev_get_gpio_in(gicdev, 101);
            }
            if (ipq->qup_spi) {
                ipq->qup_spi->irq = qdev_get_gpio_in(gicdev, 95);
            }
            ipq->gic = gicdev;

            for (n = 0; n < machine->smp.cpus; n++) {
                DeviceState *cpudev = DEVICE(qemu_get_cpu(n));
                /*
                 * GIC input lines are laid out as [SPIs][32 PPIs per CPU]
                 * (see gic_init_irqs_and_mmio), so the first PPI of CPU n is
                 * at (num_irq - 32) + n * 32, not at n * 32.
                 */
                int base = (IPQ_GIC_NUM_IRQ - GIC_INTERNAL) +
                           n * GIC_INTERNAL;
                unsigned k;

                for (k = 0; k < NUM_GTIMERS; k++) {
                    if (!timer_intid[k]) {
                        continue;
                    }
                    qdev_connect_gpio_out(cpudev, k,
                                          qdev_get_gpio_in(gicdev,
                                                           base + timer_intid[k]));
                }
                sysbus_connect_irq(SYS_BUS_DEVICE(gicdev), n,
                                   qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
                sysbus_connect_irq(SYS_BUS_DEVICE(gicdev),
                                   n + machine->smp.cpus,
                                   qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
                sysbus_connect_irq(SYS_BUS_DEVICE(gicdev),
                                   n + 2 * machine->smp.cpus,
                                   qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
                sysbus_connect_irq(SYS_BUS_DEVICE(gicdev),
                                   n + 3 * machine->smp.cpus,
                                   qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
            }
            fprintf(stderr, "ipq4019: QGIC2 at 0x%08x (num-irq %u, %d cpu)\n",
                   IPQ_QGIC_BASE, IPQ_GIC_NUM_IRQ, machine->smp.cpus);
        }
    }
    /* SoC revision / chip-id / strap registers SBL1 probes at low addresses */
    add_stub(sysmem, 0x40000, 0x1c000, "soc-low");
    /*
     * 0x5c000 / 0x5e000: the "boot context" the mask-ROM PBL leaves behind:
     * per-device descriptors at 0x5e030, flash descriptors at 0x5c0a8.  Plain
     * RAM so it can be inspected and poked with the gdbstub during bring-up.
     */
    bootctx = add_mem(sysmem, 0x5c000, 0x1000, "bootctx");
    add_mem(sysmem, 0x5d000, 0x1000, "bootctx1");
    bootdev = add_mem(sysmem, 0x5e000, 0x1000, "bootdev");
    add_stub(sysmem, 0x5f000, 0xa1000, "soc-low2");
    /*
     * USB.  Both controllers are Synopsys DWC3 cores behind a QCOM glue
     * block, and both sit on a window that the kernel maps and polls the
     * moment its dwc3 module loads -- an unmapped window is an external
     * abort, i.e. a kernel panic, not a failed probe.
     *
     * The register files exist here so the drivers can probe them the way
     * they do on the board; they read as zero, which is what an unclocked
     * core reports, so dwc3 declines the device ("not a DesignWare USB3
     * DRD Core") instead of faulting.  USB itself is not modelled yet; see
     * the "not modelled" list in README.md.
     */
    add_stub(sysmem, 0x08a00000, 0x0f8000, "usb3-dwc3");
    add_stub(sysmem, 0x08af8800, 0x000800, "usb3-qscratch");
    add_stub(sysmem, 0x06000000, 0x0f8000, "usb2-dwc3");
    add_stub(sysmem, 0x060f8800, 0x000800, "usb2-qscratch");
    /*
     * The audio subsystem (ADSS): 0x07700000..0x0771ffff holds the audio
     * global block, the four PCM mailboxes and the four stereo ports (the
     * vendor binding calls the window 0x7700000 with ADSS_RANGE 0x20000).
     * Nothing here is audio-aware, but the window has to answer: the
     * PandoraBox bootloader that ships on the 708-class boards zeros a
     * handful of ADSS registers (0x10c, 0x12c, 0x14c, ...) right after it
     * works out the flash layout, and an unmapped store there is a data
     * abort -- its handler loops on the fault forever, before the bootloader
     * ever looks at a NAND device, so the board never reaches a kernel.
     */
    add_stub(sysmem, 0x07700000, 0x00020000, "adss");

    /*
     * WLAN register windows (the integrated 802.11ac radio and its CPU).
     * The radio itself is not emulated, but its register block has to answer:
     * RouterOS loads qca_ol from modprobe, and hif_ahb_enable_radio writes
     * 0x0a04f014 early in the probe.  With no memory region behind it that
     * write is a synchronous external abort, which panics Linux and resets
     * the board before it finishes starting up.  A register file that reads
     * back what was written (and all-ones where nothing was written, like an
     * unclocked block) lets the probe fail or time out instead of faulting.
     * The windows and their sizes come from the board device tree
     * (`wifi@a000000`, `wifi@a800000`, 2 MiB each).
     */
    {
        /*
         * What an unwritten register reads back decides whether the driver
         * thinks the radio is alive.  All-ones says "every status bit set",
         * so polls for "firmware ready" succeed and the probe continues with
         * state that was never initialised; zero says "this block is not
         * running", which is what makes qca_ol give the radio up.  Knobbed so
         * the two can be compared on the same machine: default 0.
         */
        uint32_t wifi_read = env_u32("QEMU_IPQ_WIFI_READ", 0);

        add_stub_ex(sysmem, 0x0a000000, 0x00200000, "wifi0",
                    0xffffffffu)->def_read = wifi_read;
        add_stub_ex(sysmem, 0x0a800000, 0x00200000, "wifi1",
                    0xffffffffu)->def_read = wifi_read;
    }
    /* Ethernet: MDIO master at 0x90000 (ports on it are board config). */
    add_mdio(sysmem, ipq_machine_dtb(machine));
    /*
     * ESS switch register block (ports, PSGMII, PCS) and the EDMA engine.
     * Both are register files for now: the boot chain programs them and
     * polls status bits, but no packet moves until EDMA descriptors are
     * modelled.
     */
    {
        IPQRegFile *ess = add_regfile(sysmem, 0x0c000000, 0x00080000, "ess");

        /*
         * QCA8K_REG_MASK_CTRL (offset 0): device id in bits 15:8, revision in
         * bits 7:0.  The switch inside the IPQ4019 reports id 0x14, which is
         * what qca8k_read_switch_id() insists on before it will bind -- with
         * the register reading 0 the driver bails out with
         * "Switch id detected 0 but expected 14" (both %x) and no DSA ports
         * ever appear.  This is a power-on value, so it has to survive the
         * reset the machine gets before the guest runs; regfile_seal_all()
         * is what freezes it (see IPQRegFile::por).
         */
        ess->regs[0] = 0x1402;
        /* command registers whose busy bit clears when the op completes */
        regfile_clr_bit(ess, 0x034, 1u << 17);      /* QCA8K_MIB_BUSY */
        regfile_clr_bit(ess, 0x60c, 1u << 31);      /* QCA8K_ATU_FUNC_BUSY */
        regfile_clr_bit(ess, 0x614, 1u << 31);      /* QCA8K_VTU_FUNC1_BUSY */
    }
    add_edma(sysmem, 0x0c080000, 0x00080000);
    if (!getenv("QEMU_IPQ_EDMA_NO_IRQ")) {
        edma_wire_irqs(ipq->gic);
    }
    /* Attach the engine to a host backend, if the user gave us one. */
    edma_connect_netdev(ipq->edma);
    /* TCSR: SoC revision, boot option and strap registers */
    add_stub(sysmem, 0x1900000, 0x100000, "tcsr");
    /*
     * 0x08e00000..0x08e3ffff: qcrypto (0x08e20000) + crypto BAM (0x08e04000)
     * and the block SBL1 pokes during the post-CDT DDR/training step
     * (0x08e3a000..0x08e3dfff).  Decode-only for now; the bit-0 self-clearing
     * trigger mask keeps poll loops happy.
     */
    add_stub_ex(sysmem, 0x08e00000, 0x40000, "qcrypto", 0x7ffffffeu);

    /*
     * PCIe0.  The board tree puts the DW core's DBI at 0x40000000, its ELBI
     * (the same page, at 0x40000f20), the config space at 0x40100000 and the
     * downstream BARs at 0x40200000/0x40300000.  MikroTik's netinstall kernel
     * (Linux 5.6) probes pcie_qcom from its initramfs, and with the window
     * unmapped the first register read in qcom_pcie_host_init() is a
     * synchronous external abort -- "Internal error: : 8" in pci_host_init,
     * then a panic, before the installer ever runs.
     *
     * Nothing is behind the window, so it answers as an unclocked block
     * (reads 0): the core reports no link and dw_pcie_wait_for_link() times
     * out, which the driver turns into a failed probe instead of a fault.  A
     * real card in the slot is a later, separate piece of work.
     */
    {
        /*
         * The DBI as the core reports itself at power-on: Qualcomm vendor id,
         * a PCI-to-PCI bridge header (so hdr_type is 0x01, not 0xff) and the
         * bridge class code.  The installer's pcie_qcom reads these before it
         * programs the inbound window, and all-ones makes it take a path that
         * writes the iATU before it has looked at the header at all.
         */
        IPQStub *dbi = add_stub_ex(sysmem, 0x40000000, 0x00100000, "pcie0-dbi",
                                   0);

        dbi->def_read = 0;                      /* an idle core reports 0 */
        stub_pin(dbi, 0x000, 0x17cb0101u);      /* vendor 0x17cb, device 0x0101 */
        stub_pin(dbi, 0x008, 0x06040001u);      /* class: PCI bridge */
        stub_pin(dbi, 0x00c, 0x00010008u);      /* header type 0x01 = bridge */
    }
    add_stub_ex(sysmem, 0x40100000, 0x00001000, "pcie0-config", 0);
    /*
     * The root complex's own outbound windows: IO at 0x40200000 (1 MiB) and
     * MEM at 0x40300000 (13 MiB) in the board tree.  pcie_qcom writes the
     * translation registers and then touches the window itself; with no
     * device behind it a real board completes and the writes vanish, but an
     * unmapped window is an external abort -- the installer's first PCIe
     * panic was the DBI page, this is the next one, one window further in.
     * Plain RAM makes an empty window harmless.
     */
    add_stub(sysmem, 0x40200000, 0x00e00000, "pcie0-window");

    /*
     * qca-gcnt is the 32.768 kHz sleep counter.  Firmware delay loops use the
     * 48 MHz QTIMER above, so the two must not share a rate.
     */
    gcnt = g_new0(MemoryRegion, 1);
    gcnt_hz = env_u32("QEMU_IPQ_GCNT_HZ", 32768);
    memory_region_init_io(gcnt, NULL, &gcnt_ops, &gcnt_hz, "gcnt", 8);
    memory_region_add_subregion(sysmem, IPQ_GCNT_BASE, gcnt);

    /*
     * SBL1's udelay() reads a 32-bit free-running counter at 0x0b021000
     * (the APCS / qtimer frame) and scales microseconds by 19200, i.e. it
     * expects the 19.2 MHz XO.  apcs-hi is a plain stub, so carve the page
     * out of it and make it count.
     */
    xo_timer = g_new0(MemoryRegion, 1);
    xo_hz = env_u32("QEMU_IPQ_XO_HZ", 19200000);
    memory_region_init_io(xo_timer, NULL, &gcnt_ops, &xo_hz, "xo-timer", 0x1000);
    memory_region_add_subregion_overlap(sysmem, 0x0b021000, xo_timer, 1);

    /*
     * 0x0b023000: the counter SBL1's boot logger timestamps with.  It is read
     * as a plain 32-bit free-running value, and boot_log_get_time() converts
     * it with "ti -> double, / 48.0, -> u32", i.e. it is a 48 MHz TSC in
     * units of 1/48 us.  If the read returns 0xffffffff the logger treats the
     * timestamp as unavailable and prints "B - <msg>" with no time field.
     */
    tsc_timer = g_new0(MemoryRegion, 1);
    tsc_hz = env_u32("QEMU_IPQ_TSC_HZ", 48000000);
    memory_region_init_io(tsc_timer, NULL, &gcnt_ops, &tsc_hz, "tsc", 0x1000);
    memory_region_add_subregion_overlap(sysmem, 0x0b023000, tsc_timer, 1);

    /*
     * Console UARTs.  BLSP1 UART1 (0x78af000) is <GIC_SPI 107> in the DTS and
     * BLSP1 UART2 (0x78b0000) is <GIC_SPI 108>; arm_gic's input line n is
     * SPI n, same mapping as the BAM and QUP above.
     */
    add_uart(sysmem, IPQ_UART0_BASE, "uart0", serial_hd(0),
             ipq->gic ? qdev_get_gpio_in(ipq->gic, 107) : NULL);
    if (serial_hd(1)) {
        add_uart(sysmem, IPQ_UART1_BASE, "uart1", serial_hd(1),
                 ipq->gic ? qdev_get_gpio_in(ipq->gic, 108) : NULL);
    } else {
        add_stub(sysmem, IPQ_UART1_BASE, 0x1000, "uart1");
    }

    ipq4019_binfo.ram_size = machine->ram_size;
    ipq4019_binfo.loader_start = IPQ_RAM_BASE;
    ipq4019_binfo.entry = entry;
    ipq4019_binfo.firmware_loaded = machine->firmware != NULL;

    /*
     * SBL1 shares state with the PBL through OCIMEM at 0x08600000 (boot
     * handshake flag at 0x08607df4); give it a full MiB of RAM.
     */
    ipq_map_ram(sysmem, 0x08600000, 0x100000, "ipq4019.ocimem");

    /*
     * SBL1 keeps a small "cookie" block at a fixed physical address: it reads
     * *(0x08afc000), checks it against 0x00abcdef and clears it again
     * (sbl1_mc.c).  Leave it as zeroed RAM, i.e. "no cookie saved".
     */
    ipq_map_ram(sysmem, 0x08afc000, 0x1000, "ipq4019.sbl1-cookie");

    /*
     * Default boot context: one present device of type 0, and a flash
     * descriptor block the driver can be pointed at.  Values come from the
     * environment so the layout can be probed without rebuilding.
     */
    {
        uint8_t *bc = g_malloc0(0x1000);
        uint32_t devcfg = env_u32("QEMU_IPQ_DEVCFG", 0x10);

        *(uint32_t *)(bc + 0x024) = env_u32("QEMU_IPQ_BOOTDEV_NR", 0x01000000);
        *(uint32_t *)(bc + 0x030) = env_u32("QEMU_IPQ_BOOTDEV_CAP", 0x00000000);
        *(uint32_t *)(bc + 0x07c) = env_u32("QEMU_IPQ_BOOTDEV_WIN", 0x00000000);
        for (i = 0; i < 4; i++) {
            *(uint32_t *)(bc + 0x0a8 + 8 * i) =
                env_u32("QEMU_IPQ_BOOTDEV_SRC", 0x00000000);
            *(uint32_t *)(bc + 0x0ac + 8 * i) =
                env_u32("QEMU_IPQ_BOOTDEV_LEN", 0x00000000);
        }
        memcpy(bootctx->data, bc, 0x1000);
        g_free(bc);

        bc = g_malloc0(0x1000);
        for (i = 0; i < 64; i++) {
            *(uint32_t *)(bc + 0x030 + 4 * i) = devcfg;
        }
        /*
         * 0x5e02c: the "Boot Config" word SBL1 logs as "Boot Config, 0x..".
         * 0x21 is what the reference device reports (bit0 = boot device
         * valid, bit5 = SPI/NOR flash boot).
         */
        *(uint32_t *)(bc + 0x02c) = env_u32("QEMU_IPQ_BOOTCFG", 0x21);
        memcpy(bootdev->data, bc, 0x1000);
        g_free(bc);
    }

    /*
     * -kernel is content for a die that is not a board yet, never the thing
     * that boots: ipq_boot_image() reads the boot chain out of the configured
     * die.  When that die came from a store it already held one, so the image
     * was not programmed and the wrong board can be booted while the command
     * line says otherwise.  Say which of the two happened.
     */
    {
        const char *dev, *img, *store;
        bool loaded;

        ipq_boot_dev_info(&dev, &img, &store, &loaded);
        if (dev && loaded && img) {
            warn_report("the %s die (%s) already holds a boot chain, so %s "
                        "was not programmed into it; delete that file to "
                        "program from the image", dev, store, img);
        }
    }

    if (ipq4019_load_elf(machine, sysmem)) {
        /* SBL1 loaded: hand it the PBL boot-info pointer in r0 */
        uint8_t *bi = g_malloc0(0x200);

        ipq4019_bootinfo = env_u32("QEMU_IPQ_BOOTINFO", 0x08680000);
        /*
         * +0x14/+0x18: (pointer, size) of the level-1 translation table the
         * PBL hands over.  sbl1_mc.c:1119 asserts if the pointer is NULL and
         * :1138 asserts unless every region it needs has a valid descriptor
         * (bit 0 set), so describe all of them as coarse tables pointing at
         * one scratch level-2 page.
         */
        *(uint32_t *)(bi + 0x14) = ipq4019_bootinfo + 0x10000;
        *(uint32_t *)(bi + 0x18) = 0x4000;
        /*
         * +0x30: pointer to the mask-ROM service table.  SBL1 copies this
         * whole struct to 0x0a0e2100 (boot_pbl_v1.c) and later calls
         * (*(table + 0x60))(1, &out) while locating the next image, so the
         * entries are function pointers into ROM we do not have.
         */
        *(uint32_t *)(bi + 0x30) = ipq4019_bootinfo + 0x200;
        /*
         * +0x2c: PBL shared data.  boot_pbl_v1.c:158 asserts if NULL and
         * reads the boot reason from +0x18 (0 = cold boot); +0x00 points at
         * the SBL shared data block.
         */
        *(uint32_t *)(bi + 0x2c) = ipq4019_bootinfo + 0x300;
        rom_add_blob_fixed("ipq.bootinfo", bi, 0x200, ipq4019_bootinfo);
        g_free(bi);

        bi = g_malloc0(0x200);
        /*
         * +0x00 is the *boot device type* the mask ROM reports to SBL1.
         * boot_pbl_v1.c:0xa8c9398 returns *(*(0x0a0e2100 + 0x2c)), i.e. this
         * word, and boot_flash_target_init (boot_flash_target.c) switches on
         * it.  The value is the QCA flash-type enum, the same one SBL1 copies
         * into SMEM and U-Boot reads back through smem_get_boot_flash():
         *
         *   0 no flash   1 NOR   2 NAND   3 OneNAND   4 SDC   5 MMC   6 SPI
         *
         * This machine only models SPI NOR, so 6 is the correct report.  It
         * matters beyond driver selection: SMEM_BOOT_NAND_FLASH (2) makes
         * U-Boot take the NAND environment path and BUG_ON(board_env_size <
         * CONFIG_ENV_SIZE_MAX) in board_init(), while SMEM_BOOT_SPI_FLASH (6)
         * takes the SPI path, which only rejects an *oversized* env part.
         *
         * Types 0/1 skip the flash probe entirely, so SBL1 then never loads
         * the later images and falls into Sahara download mode.
         */
        *(uint32_t *)(bi + 0x000) =
            env_u32("QEMU_IPQ_BOOTDEV_TYPE",
                    ipq_cfg.boot == IPQ_BOOT_SPI_NOR ||
                    ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? IPQ_FLASH_TYPE_SPI :
                    IPQ_FLASH_TYPE_NONE);
        *(uint32_t *)(bi + 0x018) = 0;         /* cold boot */
        rom_add_blob_fixed("ipq.pbl", bi, 0x200, ipq4019_bootinfo + 0x300);
        g_free(bi);

        /*
         * Mask-ROM service table: the PBL hands SBL1 a list of ROM entry
         * points.  We have no mask ROM, so synthesise a Thumb stub per slot
         * SBL1 actually calls.  Each stub does
         *
         *      movs r0, #<ret>        ; status
         *      cmp  r1, #0
         *      it   ne                (encoded as a conditional store)
         *      strne r0, [r1, #0]     ; *out = <ret>
         *      bx   lr
         *
         * Stubs live at bootinfo+0x2000 so a PC in that range in the exec
         * trace identifies which slot was reached.
         */
        {
            static const struct { uint32_t off, env_default; const char *env; }
            slots[] = {
                { 0x60, 0, "QEMU_IPQ_PBL_SVC_60" },
            };
            uint32_t stub_base = ipq4019_bootinfo + 0x2000;

            bi = g_malloc0(0x100);
            for (i = 0; i < ARRAY_SIZE(slots); i++) {
                uint8_t code[10] = {
                    0x00, 0x20,         /* movs r0, #imm        */
                    0x00, 0x29,         /* cmp  r1, #0          */
                    0xbf, 0x18,         /* it   ne              */
                    0x08, 0x60,         /* strne r0, [r1]       */
                    0x70, 0x47,         /* bx   lr              */
                };
                uint32_t ret = env_u32(slots[i].env, slots[i].env_default);
                uint32_t addr = stub_base + 16 * i;

                code[1] = ret & 0xff;
                rom_add_blob_fixed("ipq.pblstub", code, sizeof(code), addr);
                *(uint32_t *)(bi + slots[i].off) = addr | 1;
            }
            rom_add_blob_fixed("ipq.pblops", bi, 0x100,
                               ipq4019_bootinfo + 0x200);
            g_free(bi);
        }

        bi = g_malloc0(0x4000);
        /*
         * The PBL hands over a level-1 table.  sbl1_mc.c:1138 checks the
         * regions listed in SBL1's own region table (0xa8d7b80) and requires
         * a *coarse table* descriptor (bit 0 set) for each of them, while
         * sbl1_mc.c:696 panics if more than 14 of the 4096 entries are
         * coarse.  Those regions are SBL1's own segments, so only L1 index
         * 0x086 and 0x0a8 need level-2 tables; everything else is covered by
         * 1 MiB section descriptors (normal memory for DRAM, device for MMIO).
         */
        for (i = 0; i < 0x1000; i++) {
            bool normal = (i >= 0x080 && i < 0x100) || i >= 0x800;

            ((uint32_t *)bi)[i] = (i << 20) |
                                  (normal ? 0x60eu : 0x606u);
        }
        ((uint32_t *)bi)[0x086] = (ipq4019_bootinfo + 0x20000) | 1;
        ((uint32_t *)bi)[0x0a8] = (ipq4019_bootinfo + 0x21000) | 1;
        rom_add_blob_fixed("ipq.bootinfo.ttb", bi, 0x4000,
                           ipq4019_bootinfo + 0x10000);
        g_free(bi);

        /* level-2 tables for those two sections: 4 KiB pages, normal memory */
        bi = g_malloc0(0x2000);
        for (i = 0; i < 0x200; i++) {
            ((uint32_t *)bi)[i] = (0x08600000 + i * 0x1000) | 0x3e;
            ((uint32_t *)bi)[0x200 + i] = (0x0a800000 + i * 0x1000) | 0x3e;
        }
        rom_add_blob_fixed("ipq.bootinfo.l2", bi, 0x2000,
                           ipq4019_bootinfo + 0x20000);
        g_free(bi);
        fprintf(stderr, "ipq4019: boot-info handed to SBL1 in r0 = 0x%08x\n",
               ipq4019_bootinfo);
    } else {
        /*
         * No SBL ELF in the boot device.  A die that was loaded from a store
         * and a -kernel that could not be used are both mistakes worth naming:
         * the alternative is a machine that runs an empty boot ROM and looks
         * like a hang.
         */
        const char *dev, *img, *store;
        bool loaded;

        ipq_boot_dev_info(&dev, &img, &store, &loaded);
        if (dev && store && (machine->kernel_filename || loaded)) {
            error_report("ipq4019: the %s die (%s) holds no SBL ELF, so there "
                         "is nothing to boot there; delete that file to program "
                         "it from an image", dev, store);
            if (machine->kernel_filename) {
                error_report("ipq4019: -kernel %s is a programming source, not "
                             "a boot source: it is only copied into an empty "
                             "die", machine->kernel_filename);
            }
            exit(1);
        }
        arm_load_kernel(ARM_CPU(first_cpu), machine, &ipq4019_binfo);
    }

    /*
     * Everything the machine programmed into a register file during init is
     * the hardware's power-on state.  Freeze it before the first reset, which
     * happens as the machine starts and would otherwise zero it.
     */
    regfile_seal_all();

    /*
     * The QSEE handover policy, if the image has a QSEE stage.  Installed
     * before the machine starts, and after the CPUs exist.
     */
    ipq4019_arm_qsee_policy();

    /*
     * Registered after arm_load_kernel() so that it runs after the CPUs'
     * own reset handlers, which clear CP15 state.
     */
    for (CPUState *cs = first_cpu; cs; cs = CPU_NEXT(cs)) {
        qemu_register_reset(ipq4019_reset, ARM_CPU(cs));
    }
    qemu_register_reset(ipq4019_dev_reset, NULL);
}

/*
 * -machine ipq4019,<property>=<value>
 *
 * Every one of these only *records* what was asked for; ipq_board_config_resolve()
 * merges it with the preset and the deprecated environment knobs when the
 * machine starts.  That keeps `board=kt708,nor=off` and `nor=off,board=kt708`
 * meaning the same thing, and lets the same property set describe a board that
 * has no preset at all.
 */
typedef enum IPQPropKind {
    IPQ_PROP_BOOL,
    IPQ_PROP_UINT32,
    IPQ_PROP_SIZE,              /* string, parsed with the usual 16M suffixes */
    IPQ_PROP_STR,
} IPQPropKind;

typedef struct IPQBoardProp {
    const char *name;
    size_t off;                 /* field in ipq_cfg_pending */
    uint32_t bit;               /* which IPQ_CFG_* it sets */
    IPQPropKind kind;
} IPQBoardProp;

static void ipq_prop_bool(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    const IPQBoardProp *p = opaque;
    bool *field = (bool *)((char *)&ipq_cfg_pending + p->off);

    if (!visit_type_bool(v, name, field, errp)) {
        return;
    }
    ipq_cfg_set |= p->bit;
}

static void ipq_prop_uint32(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    const IPQBoardProp *p = opaque;
    uint32_t *field = (uint32_t *)((char *)&ipq_cfg_pending + p->off);

    if (!visit_type_uint32(v, name, field, errp)) {
        return;
    }
    ipq_cfg_set |= p->bit;
}

/*
 * Sizes are strings so that the usual QEMU suffixes work: nor-size=32M reads
 * the way -m 256M does, instead of demanding 33554432.
 */
static void ipq_prop_size(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    const IPQBoardProp *p = opaque;
    uint32_t *field = (uint32_t *)((char *)&ipq_cfg_pending + p->off);
    char *s = NULL;
    uint64_t val;

    if (!visit_type_str(v, name, &s, errp)) {
        return;
    }
    if (qemu_strtosz(s, NULL, &val) < 0 || val > UINT32_MAX) {
        error_setg(errp, "%s='%s' is not a size", name, s);
        g_free(s);
        return;
    }
    g_free(s);
    *field = val;
    ipq_cfg_set |= p->bit;
}

static void ipq_prop_str(Object *obj, Visitor *v, const char *name,
                         void *opaque, Error **errp)
{
    const IPQBoardProp *p = opaque;
    const char **field = (const char **)((char *)&ipq_cfg_pending + p->off);
    char *s = NULL;

    if (!visit_type_str(v, name, &s, errp)) {
        return;
    }
    g_free((char *)*field);
    *field = g_strdup(s);
    g_free(s);
    ipq_cfg_set |= p->bit;
}

static void ipq_prop_boot(Object *obj, Visitor *v, const char *name,
                          void *opaque, Error **errp)
{
    char *s = NULL;

    if (!visit_type_str(v, name, &s, errp)) {
        return;
    }
    if (g_strcmp0(s, "none") == 0) {
        ipq_cfg_pending.boot = IPQ_BOOT_NONE;
    } else if (g_strcmp0(s, "spi-nor") == 0) {
        ipq_cfg_pending.boot = IPQ_BOOT_SPI_NOR;
    } else if (g_strcmp0(s, "spi-nand") == 0) {
        ipq_cfg_pending.boot = IPQ_BOOT_SPI_NAND;
    } else if (g_strcmp0(s, "qpic-nand") == 0) {
        ipq_cfg_pending.boot = IPQ_BOOT_QPIC_NAND;
    } else {
        error_setg(errp, "boot-device='%s' is not one of none, spi-nor, "
                   "spi-nand, qpic-nand", s);
        g_free(s);
        return;
    }
    g_free(s);
    ipq_cfg_set |= IPQ_CFG_BOOT;
}

static char *ipq_prop_board_get(Object *obj, Error **errp)
{
    return g_strdup(ipq_cfg_board ? ipq_cfg_board : "default");
}

static void ipq_prop_board_set(Object *obj, const char *value, Error **errp)
{
    g_free((char *)ipq_cfg_board);
    ipq_cfg_board = g_strdup(value);
}

static void ipq4019_machine_add_props(MachineClass *mc)
{
    static const IPQBoardProp props[] = {
        { "nor",        offsetof(IPQBoardConfig, nor),        IPQ_CFG_NOR, IPQ_PROP_BOOL },
        { "nor-size",   offsetof(IPQBoardConfig, nor_size),   IPQ_CFG_NOR_SIZE, IPQ_PROP_SIZE },
        { "nor-id",     offsetof(IPQBoardConfig, nor_id),     IPQ_CFG_NOR_ID, IPQ_PROP_STR },
        { "nor-uid",    offsetof(IPQBoardConfig, nor_uid),    IPQ_CFG_NOR_UID, IPQ_PROP_STR },
        { "nor-file",   offsetof(IPQBoardConfig, nor_file),   IPQ_CFG_NOR_FILE, IPQ_PROP_STR },
        { "nor-drive",  offsetof(IPQBoardConfig, nor_drive),  IPQ_CFG_NOR_DRIVE, IPQ_PROP_STR },
        { "nand",       offsetof(IPQBoardConfig, nand),       IPQ_CFG_NAND, IPQ_PROP_BOOL },
        { "nand-size",  offsetof(IPQBoardConfig, nand_size),  IPQ_CFG_NAND_SIZE, IPQ_PROP_SIZE },
        { "nand-id",    offsetof(IPQBoardConfig, nand_id),    IPQ_CFG_NAND_ID, IPQ_PROP_STR },
        { "nand-file",  offsetof(IPQBoardConfig, nand_file),  IPQ_CFG_NAND_FILE, IPQ_PROP_STR },
        { "nand-drive", offsetof(IPQBoardConfig, nand_drive), IPQ_CFG_NAND_DRIVE, IPQ_PROP_STR },
        { "qpic-nand",  offsetof(IPQBoardConfig, qpic_nand),
          IPQ_CFG_QPIC_NAND, IPQ_PROP_BOOL },
        { "qpic-nand-size", offsetof(IPQBoardConfig, qpic_nand_size),
          IPQ_CFG_QPIC_NAND_SIZE, IPQ_PROP_SIZE },
        { "qpic-nand-id", offsetof(IPQBoardConfig, qpic_nand_id),
          IPQ_CFG_QPIC_NAND_ID, IPQ_PROP_STR },
        { "qpic-nand-file", offsetof(IPQBoardConfig, qpic_nand_file),
          IPQ_CFG_QPIC_NAND_FILE, IPQ_PROP_STR },
        { "qpic-nand-drive", offsetof(IPQBoardConfig, qpic_nand_drive),
          IPQ_CFG_QPIC_NAND_DRIVE, IPQ_PROP_STR },
        { "nor-cs-gpio",  offsetof(IPQBoardConfig, nor_cs_gpio),
          IPQ_CFG_NOR_CS, IPQ_PROP_UINT32 },
        { "nand-cs-gpio", offsetof(IPQBoardConfig, nand_cs_gpio),
          IPQ_CFG_NAND_CS, IPQ_PROP_UINT32 },
        { "leds",       offsetof(IPQBoardConfig, leds),       IPQ_CFG_LEDS, IPQ_PROP_STR },
        { "keys",       offsetof(IPQBoardConfig, keys),       IPQ_CFG_KEYS, IPQ_PROP_STR },
        { "eth-ports",  offsetof(IPQBoardConfig, eth_ports),  IPQ_CFG_ETH_PORTS, IPQ_PROP_STR },
        { "eth-bridge-port", offsetof(IPQBoardConfig, eth_bridge_port),
          IPQ_CFG_ETH_BRIDGE, IPQ_PROP_UINT32 },
        { "eth-phy-id", offsetof(IPQBoardConfig, eth_phy_id),
          IPQ_CFG_ETH_PHY_ID, IPQ_PROP_UINT32 },
        { "poweroff",   offsetof(IPQBoardConfig, poweroff),   IPQ_CFG_POWEROFF, IPQ_PROP_BOOL },
    };
    static const char *const desc[ARRAY_SIZE(props)] = {
        "attach the SPI NOR die on chip select 0 (on/off)",
        "NOR die size in bytes (power of two, 1M..64M)",
        "NOR RDID as hex, e.g. ef4019",
        "NOR Unique-ID as hex, 8 bytes, e.g. 0141ac3357ac0001",
        "file that is the NOR part (survives a power cut)",
        "block backend id that is the NOR part, e.g. from -drive if=none",
        "attach the SPI NAND die on chip select 1 (on/off)",
        "NAND die size in bytes, a whole number of erase blocks",
        "NAND RDID as hex, e.g. c212",
        "file that is the NAND part (survives a power cut)",
        "block backend id that is the NAND part, e.g. from -drive if=none",
        "attach the parallel NAND die behind the QPIC controller (on/off)",
        "parallel NAND die size in bytes, a whole number of erase blocks",
        "parallel NAND id as hex, e.g. c2f1801d",
        "file that is the parallel NAND part (survives a power cut)",
        "block backend id that is the parallel NAND part",
        "TLMM pin driving the NOR chip select",
        "TLMM pin driving the NAND chip select",
        "LEDs as name:pin[:al];... (shown in QEMU's window; ';' separates)",
        "keys as name:pin[:al]:qcode;... (window or QMP send-key)",
        "switch PHYs as addr[:speed[:id]];... ('!' = link down)",
        "switch port wired to the host -netdev (1..6, default lan1=5)",
        "PHY identifier for ports that do not name their own",
        "a CPU parked with IRQs masked powers the board off (on/off)",
    };
    ObjectClass *oc = OBJECT_CLASS(mc);
    size_t i;

    object_class_property_add_str(oc, "board", ipq_prop_board_get,
                                  ipq_prop_board_set);
    object_class_property_set_description(oc, "board",
        "board preset: default, none, hap-ac2 (or a command-line "
        "property set)");
    /*
     * "boot" already exists on MachineState (the structured -machine boot=
     * configuration), so the strap is named for what it is.
     */
    object_class_property_add(oc, "boot-device", "string", NULL, ipq_prop_boot,
                              NULL, NULL);
    object_class_property_set_description(oc, "boot-device",
        "which die the boot chain comes from: none, spi-nor, spi-nand");

    for (i = 0; i < ARRAY_SIZE(props); i++) {
        void (*set)(Object *, Visitor *, const char *, void *, Error **);
        const char *type;

        switch (props[i].kind) {
        case IPQ_PROP_BOOL:
            set = ipq_prop_bool;
            type = "bool";
            break;
        case IPQ_PROP_UINT32:
            set = ipq_prop_uint32;
            type = "uint32";
            break;
        case IPQ_PROP_SIZE:
        case IPQ_PROP_STR:
            set = props[i].kind == IPQ_PROP_SIZE ? ipq_prop_size : ipq_prop_str;
            type = "string";
            break;
        default:
            g_assert_not_reached();
        }

        object_class_property_add(oc, props[i].name, type, NULL, set,
                                  NULL, (void *)&props[i]);
        object_class_property_set_description(oc, props[i].name, desc[i]);
    }
}

static void ipq4019_machine_init(MachineClass *mc)
{
    mc->desc = "Qualcomm IPQ4018/IPQ4019 (ipq40xx) bring-up board";
    mc->init = ipq4019_init;
    mc->max_cpus = 4;
    mc->default_cpus = 1;
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "ipq4019.dram";
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    ipq4019_machine_add_props(mc);
}

DEFINE_MACHINE_ARM("ipq4019", ipq4019_machine_init)
