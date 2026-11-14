/*
 * Qualcomm IPQ4018/IPQ4019 -- the flash stack: BLSP1 QUP SPI with a NOR
 * die behind chip select 0, a SPI NAND behind chip select 1, and the
 * backing store both share with the parallel NAND.
 *
 * The QUP is modelled register by register because both boot chains program
 * it differently (PIO vs BAM, FORCE_CS vs a plain GPIO select); the dies are
 * byte images with their own command sets, and a die's store is either a file
 * or a block backend so programming survives the machine exiting.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "target/arm/cpu-qom.h"
#include "cpu.h"
#include "system/memory.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "ipq4019.h"

/* The die command sets are defined with their blocks below; the constructors
 * at the end of this file use them, so declare them up front. */
static void nand_xact_reset(void);
static void nand_opcode(IPQQupl *q, uint8_t op);
static void nand_address_done(IPQQupl *q);
static void nand_data_byte(uint8_t b);
static uint8_t nand_miso(IPQQupl *q);
static bool nand_persist_open(IPQNand *n, const char *path, const char *drive,
                              const char *image);
static void nand_persist_snapshot(IPQNand *n);


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



uint32_t ipq_flash_die_size = IPQ_FLASH_DEFAULT;
uint8_t ipq_flash_jedec[4] = { 0xef, 0x40, 0x19, 0x00 };
int ipq_flash_jedec_len = 3;
uint8_t ipq_flash_uid[8];
int ipq_flash_uid_len = 8;

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

void die_dbg_note(uint8_t v)
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


bool ipq_die_store_open(IPQDieStore *s)
{
    return s->fd >= 0 || s->blk;
}

bool ipq_die_store_writable(IPQDieStore *s)
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
bool ipq_die_store_pread(IPQDieStore *s, void *dst, uint64_t len,
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

void ipq_die_store_pwrite(IPQDieStore *s, const void *src, uint32_t len,
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
void ipq_die_store_sync(IPQDieStore *s)
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
uint64_t ipq_die_store_bind(IPQDieStore *s, const char *path,
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
bool ipq_die_store_blank(const void *buf, uint64_t len)
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




/* Erased silicon is 0xff; the factory state of this part is "all erased". */
void nand_fill_erased(uint8_t *p, uint32_t len)
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

void ipq_die_program_image(uint8_t *data, uint32_t bytes,
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
void ipq_board_add_nor(MemoryRegion *sysmem)
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
void ipq_board_add_nand(void)
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

void ipq_cs_line(void *opaque, int level)
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

IPQQupl *add_qupl(MemoryRegion *parent, hwaddr base, hwaddr size,
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


/* whether each die was loaded from its store instead of programmed */
bool ipq_nor_store_loaded;
bool ipq_nand_store_loaded;
