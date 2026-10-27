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
 *   0x004ab000  pshold          (stub)
 *   0x01000000  TLMM            (stub, shadowed)
 *   0x01800000  GCC             (stub, shadowed)
 *   0x01940000  TCSR            (stub)
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
 *   QEMU_IPQ_BOOTINFO    address of the synthesized PBL shared data handed to
 *                        SBL1 in r0 (default 0x08680000)
 *   QEMU_IPQ_BOOTCFG     value of the SoC boot-config word (0x5e02c, bits 9:0)
 *   QEMU_IPQ_DEVCFG      value of the boot-device descriptor words (0x5e030+)
 *   QEMU_IPQ_SPI_LOG=1   log the decoded SPI command stream (-d unimp)
 *   QEMU_IPQ_FLASH_SIZE  SPI NOR die size in bytes, power of two
 *                        1 MiB..64 MiB (default 32 MiB)
 *   QEMU_IPQ_FLASH_JEDEC RDID bytes as hex, e.g. "ef4019" or "20ba19"
 *                        (default: Winbond "ef40" + log2(size))
 *   QEMU_IPQ_BAM_VER     BAM version read back from +0x1000 (default 7)
 *   QEMU_IPQ_BAM_EVENT   PIPE_EVENT bits raised when a transfer completes
 *                        (default 0x100000)
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
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "chardev/char-fe.h"
#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/loader.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/device_tree.h"
#include "cpu.h"
#include <libfdt.h>

#define IPQ_ROM_BASE      0x00000000u
#define IPQ_ROM_SIZE      0x00040000u
#define IPQ_PRNG_BASE     0x00022000u
#define IPQ_GCNT_BASE     0x004a1000u
#define IPQ_PSHOLD_BASE   0x004ab000u
#define IPQ_TLMM_BASE     0x01000000u
#define IPQ_TLMM_SIZE     0x00300000u
#define IPQ_GCC_BASE      0x01800000u
#define IPQ_GCC_SIZE      0x00060000u
#define IPQ_UART0_BASE    0x078af000u
#define IPQ_UART1_BASE    0x078b0000u
#define IPQ_QUP_BASE      0x078b5000u
#define IPQ_BLSP1_BAM_BASE 0x07884000u
#define IPQ_BLSP1_BAM_SIZE 0x00023000u
#define IPQ_QPIC_BASE     0x07980000u
#define IPQ_QPIC_SIZE     0x00040000u
#define IPQ_RAM_BASE      0x80000000u
#define IPQ_QGIC_BASE     0x0b000000u
#define IPQ_FLASH_BASE    0x0e000000u
#define IPQ_FLASH_SIZE    (64 * MiB)      /* address window */
#define IPQ_FLASH_MIN     (1 * MiB)       /* smallest die modelled */
#define IPQ_FLASH_DEFAULT (32 * MiB)      /* W25Q256, the common ipq40xx part */

static uint32_t env_u32(const char *name, uint32_t def)
{
    const char *s = getenv(name);

    if (!s || !*s) {
        return def;
    }
    return strtoul(s, NULL, 0);
}

static bool env_true(const char *name)
{
    return env_u32(name, 0) != 0;
}

/* ------------------------------------------------------------------ */
/* generic shadowed stub                                               */
/* ------------------------------------------------------------------ */

typedef struct IPQStub {
    MemoryRegion mr;
    const char *name;
    uint32_t *shadow;
    uint8_t *valid;
    uint32_t nregs;
    /*
     * Bits that never read back as written.  GCC's clock-branch registers use
     * bit 0 as a write-only trigger: software sets it and then polls until it
     * reads 0, which never happens with plain shadow storage.
     */
    uint32_t read_mask;
    /*
     * Value returned for registers that were never written.  A real Qualcomm
     * register decoder answers 0 for an unimplemented offset and most registers
     * reset to 0, so 0 is the faithful default.  Poles (PLL lock, "block
     * ready") are the exception: software polls them and needs the bit set, so
     * a few blocks keep the ~0 default.
     */
    uint32_t def_read;
    /*
     * Status registers whose value is reported rather than stored: things the
     * guest waits for that this machine completes instantly (a PLL locking, a
     * block leaving reset).  Reads always return the pinned value, writes are
     * ignored -- exactly what hardware does for a read-only status field.
     */
    struct {
        uint32_t off;
        uint32_t val;
    } pin[8];
    int npin;
} IPQStub;

static void stub_pin(IPQStub *s, uint32_t off, uint32_t val)
{
    assert(s->npin < ARRAY_SIZE(s->pin));

    s->pin[s->npin].off = off;
    s->pin[s->npin].val = val;
    s->npin++;
}

static uint32_t stub_def_read(const IPQStub *s)
{
    const char *e = getenv("QEMU_IPQ_STUB_READ");

    return (e && *e) ? strtoul(e, NULL, 0) : s->def_read;
}

static const char *trace_pc(void)
{
    static char buf[32];
    CPUState *cs = current_cpu;

    if (!cs) {
        return "";
    }
    snprintf(buf, sizeof(buf), " pc=%08x",
             (uint32_t)ARM_CPU(cs)->env.regs[15]);
    return buf;
}

static uint64_t stub_read(void *opaque, hwaddr off, unsigned size)
{
    IPQStub *s = opaque;
    uint32_t idx = off >> 2;
    const char *tag = "(default) ";
    uint32_t v = 0;
    bool pinned = false;
    int i;

    for (i = 0; i < s->npin; i++) {
        if (s->pin[i].off == off) {
            v = s->pin[i].val;
            tag = "(pinned) ";
            pinned = true;
            break;
        }
    }
    if (!pinned) {
        if (idx >= s->nregs) {
            v = stub_def_read(s);
        } else if (s->valid[idx]) {
            /*
             * Written register.  read_mask models write-only bits: the clock
             * blocks use bit 0 as a self-clearing trigger (software sets it and
             * polls until it reads 0) and bit 31 as a busy flag that stays low,
             * so neither reads back as stored.
             */
            v = s->shadow[idx] & s->read_mask;
            tag = "";
        } else {
            v = stub_def_read(s);
        }
    }
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: read  off 0x%03" HWADDR_PRIx
                      " -> %s0x%08x%s\n", s->name, off, tag, v,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    return v;
}

static void stub_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQStub *s = opaque;
    uint32_t idx = off >> 2;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", s->name, off, val,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    if (s->nregs && idx < s->nregs) {
        s->shadow[idx] = val;
        s->valid[idx] = 1;
    }
}

static const MemoryRegionOps stub_ops = {
    .read = stub_read,
    .write = stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* 8 so that LDRD/STRD pairs are split into two register accesses */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static IPQStub *add_stub_ex(MemoryRegion *parent, hwaddr base, hwaddr size,
                            const char *name, uint32_t read_mask)
{
    IPQStub *s = g_new0(IPQStub, 1);

    s->name = name;
    s->read_mask = read_mask;
    s->def_read = 0xffffffffu;
    /* real register windows get shadow storage; big decode windows do not */
    s->nregs = size > 0x100000 ? 0 : size >> 2;
    if (s->nregs) {
        s->shadow = g_new0(uint32_t, s->nregs);
        s->valid = g_new0(uint8_t, s->nregs);
    }
    memory_region_init_io(&s->mr, NULL, &stub_ops, s, name, size);
    memory_region_add_subregion(parent, base, &s->mr);
    return s;
}

static void add_stub(MemoryRegion *parent, hwaddr base, hwaddr size,
                     const char *name)
{
    add_stub_ex(parent, base, size, name, 0xffffffffu);
}

/* ------------------------------------------------------------------ */
/* Traced memory window (boot flash, PBL boot context)                 */
/* ------------------------------------------------------------------ */

/*
 * The boot flash at 0xe0000000, plus the 0x5c000/0x5e000 blocks the mask-ROM
 * PBL leaves behind: boot-device descriptors, the flash window base and the
 * {source, length} read descriptors.  Kept as a plain byte array (no SPI/NAND
 * protocol model) because SBL1's boot_flash_read() is a memcpy out of these.
 */
typedef struct IPQMem {
    MemoryRegion mr;
    const char *name;
    uint8_t *data;
    uint64_t size;
} IPQMem;

/*
 * Clock rates handed to the gcnt/xo read handler as its opaque pointer.  These
 * must outlive ipq4019_init(), so they are file-scope rather than locals (a
 * dangling pointer here silently rescales every timer read).
 */
static uint32_t gcnt_hz;
static uint32_t qtimer_hz;
static uint32_t xo_hz;
static uint32_t tsc_hz;

static uint64_t mem_read(void *opaque, hwaddr off, unsigned size)
{
    IPQMem *m = opaque;
    uint64_t v;

    if (off + size > m->size) {
        v = (size == 8) ? 0xffffffffffffffffull : 0xffffffffu;
    } else {
        v = 0;
        memcpy(&v, m->data + off, size);
    }
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: read  off 0x%05" HWADDR_PRIx
                      " len %u -> 0x%08" PRIx64 "%s\n", m->name, off, size, v,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    return v;
}

static void mem_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQMem *m = opaque;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: write off 0x%05" HWADDR_PRIx
                      " <- 0x%08" PRIx64 " len %u%s\n", m->name, off, val,
                      size,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    if (off + size <= m->size) {
        memcpy(m->data + off, &val, size);
    }
}

static const MemoryRegionOps mem_ops = {
    .read = mem_read,
    .write = mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static IPQMem *add_mem(MemoryRegion *parent, hwaddr base, uint64_t size,
                       const char *name)
{
    IPQMem *m = g_new0(IPQMem, 1);

    m->name = name;
    m->size = size;
    m->data = g_malloc0(size);
    memory_region_init_io(&m->mr, NULL, &mem_ops, m, name, size);
    memory_region_add_subregion(parent, base, &m->mr);
    return m;
}

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
#define QUP_REG_STATE        0x004
#define QUP_REG_OPERATIONAL  0x018
#define QUP_REG_IO_CONTROL   0x304
#define QUP_REG_ERROR_FLAGS  0x01c
#define QUP_REG_SW_RESET     0x00c
#define QUP_OUT_FIFO         0x110
#define QUP_IN_FIFO          0x218

/* QUP_IO_CONTROL: the driver holds chip select with FORCE_CS (qca_qup_spi.h) */
#define QUP_IO_CONTROL_FORCE_CS 0x800

/* QUP_OPERATIONAL bits this model has to answer */
#define QUP_OP_IN_FIFO_FULL  0x08   /* input FIFO full (never) */
#define QUP_OP_OUT_FIFO_FULL 0x40   /* output FIFO full (never) */
#define QUP_OP_IN_AVAILABLE  0x20   /* data available in the input FIFO */

typedef struct IPQQupl {
    MemoryRegion mr;
    uint32_t shadow[0x1000 / 4];

    /* SPI slave state machine (driven by every byte clocked out) */
    int phase;              /* 0 opcode, 1 address, 2 dummy, 3 data */
    uint8_t opcode;
    uint32_t addr;
    int addr_left;
    int dummy_left;
    int cs;                 /* die selected (QUP_IO_CONTROL FORCE_CS) */
    int read_data;          /* stream data out of the flash */
    uint32_t rx_off;        /* next flash offset of the data stream */
    uint8_t rx_buf[8];      /* short replies: JEDEC id, status */
    int rx_len;
    int rx_pos;
    uint8_t status;         /* SR1: bit0 busy, bit1 write-enable latch */
    int write_sr;           /* next data byte lands in SR1 */
    uint32_t data_bytes;    /* bytes shifted out of this command's reply */
    uint32_t last_addr;     /* decoded address of the previous command */

    /* MISO queue: one entry per byte clocked out, popped by FIFO reads */
    uint8_t resp[4096];
    int resp_head;
    int resp_len;
    uint64_t clocked;
} IPQQupl;

static IPQMem *ipq_flash;
static uint32_t ipq_flash_die_size = IPQ_FLASH_DEFAULT;
static uint8_t ipq_flash_jedec[4] = { 0xef, 0x40, 0x19, 0x00 };
static int ipq_flash_jedec_len = 3;
static IPQQupl *qup_spi;         /* the one QUP the BAM pipes are wired to */

static uint8_t qup_flash_byte(uint32_t off)
{
    if (!ipq_flash || off >= ipq_flash->size) {
        return 0xff;
    }
    return ipq_flash->data[off];
}

static void qup_spi_opcode(IPQQupl *q, uint8_t op)
{
    q->opcode = op;
    q->addr_left = 0;
    q->dummy_left = 0;
    q->read_data = 0;
    q->rx_len = 0;
    q->rx_pos = 0;
    q->rx_off = 0;
    memset(q->rx_buf, 0, sizeof(q->rx_buf));

    if (env_true("QEMU_IPQ_SPI_LOG")) {
        qemu_log_mask(LOG_UNIMP, "spi-op 0x%02x (prev 0x%02x addr 0x%06x len %u)\n",
                      op, q->opcode, q->last_addr, q->data_bytes);
    }
    q->data_bytes = 0;

    switch (op) {
    case 0x03:                          /* READ */
    case 0x13:                          /* READ4 */
        q->addr_left = 3;
        q->read_data = 1;
        break;
    case 0x0b:                          /* FAST_READ */
        q->addr_left = 3;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0x0c:                          /* FAST_READ4 */
        q->addr_left = 4;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0x5a:                          /* SFDP: 24-bit address + 1 dummy */
        q->addr_left = 3;
        q->dummy_left = 1;
        q->read_data = 1;
        break;
    case 0x9f:                          /* RDID */
        /* Whatever die the board has: QEMU_IPQ_FLASH_JEDEC/_SIZE. */
        memcpy(q->rx_buf, ipq_flash_jedec, ipq_flash_jedec_len);
        q->rx_len = ipq_flash_jedec_len;
        break;
    case 0x4b:                          /* Read Unique ID: 8 bytes */
        q->rx_len = 8;
        break;
    case 0x05:                          /* RDSR1 */
        q->rx_buf[0] = q->status;
        q->rx_len = 1;
        break;
    case 0x35:                          /* RDSR2 */
    case 0x15:                          /* RDCR */
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
    case 0x01:                          /* WRSR: next data byte is SR1 */
    case 0x31:
        q->write_sr = 1;
        break;
    case 0x02:                          /* PP */
    case 0x20:                          /* SE */
    case 0x52:                          /* BE32 */
    case 0xd8:                          /* BE64 */
    case 0xc7:                          /* CE */
    case 0x60:
        q->status &= ~0x02;             /* the latch is consumed by the op */
        break;
    default:
        /* unknown opcode: no data phase, accept and ignore */
        break;
    }
}

static void qup_spi_byte(IPQQupl *q, uint8_t b)
{
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
         * Data/program phase.  Programming the die is not modelled, but the
         * status register has to take the value: SBL1 clears a block-protect
         * bit during init and then reads it back.
         */
        if (q->write_sr) {
            q->status = b & ~0x02;      /* WEL self-clears */
            q->write_sr = 0;
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
    if (q->read_data) {
        q->data_bytes++;
        return qup_flash_byte(q->rx_off++ & (ipq_flash_die_size - 1));
    }
    if (q->rx_pos < q->rx_len) {
        return q->rx_buf[q->rx_pos++];
    }
    /*
     * Past the end of a short reply (RDID, RDSR, ...) the die keeps shifting
     * out zeros.  The driver reads a fixed 4-byte device id, so this byte is
     * part of the id word it looks up.
     */
    return 0x00;
}

/* One byte out on MOSI means one byte in on MISO, as on real hardware. */
static void qup_clock(IPQQupl *q, uint8_t mosi)
{
    uint8_t miso = qup_miso(q);

    if (q->resp_head + q->resp_len < (int)sizeof(q->resp)) {
        q->resp[q->resp_head + q->resp_len] = miso;
        q->resp_len++;
    }
    q->clocked++;
    qup_spi_byte(q, mosi);
}

static uint32_t qup_in_word(IPQQupl *q)
{
    uint32_t v = 0xff;               /* FIFO idle level */

    if (q->resp_len > 0) {
        v = q->resp[q->resp_head];
        q->resp_head++;
        q->resp_len--;
        if (q->resp_len == 0) {
            q->resp_head = 0;
        }
    }
    return v;
}

static uint32_t qup_operational(IPQQupl *q)
{
    /* only input-available matters: room in/out is always reported */
    return q->resp_len > 0 ? QUP_OP_IN_AVAILABLE : 0;
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
        v = 0;
        break;
    case QUP_REG_SW_RESET:
        v = 0;                     /* self-clearing */
        break;
    case QUP_IN_FIFO:
    case QUP_IN_FIFO + 4:
    case QUP_IN_FIFO + 8:
    case QUP_IN_FIFO + 0xc:
        v = qup_in_word(q);
        break;
    default:
        v = idx < ARRAY_SIZE(q->shadow) ? q->shadow[idx] : 0;
        break;
    }
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "qup-spi: read  off 0x%03" HWADDR_PRIx
                      " -> 0x%08x%s\n", off, v,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
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
    q->rx_off = 0;
    q->rx_len = 0;
    q->rx_pos = 0;
    qup_fifo_flush(q);
}

static void qupl_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQQupl *q = opaque;
    uint32_t idx = off >> 2;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "qup-spi: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", off, val,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
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
        int assert = (val & QUP_IO_CONTROL_FORCE_CS) != 0;

        if (assert != q->cs) {
            q->cs = assert;
            qup_bus_reset(q);
        }
    }
    if (off == QUP_REG_STATE && (val & 0x3) == 0) {
        /*
         * QUP state RESET flushes the input FIFO on real hardware.  The die
         * itself only loses the transaction when chip select drops with it;
         * while FORCE_CS holds CS low there is no CS edge, so a reset between
         * the command and data phase (U-Boot does exactly that) keeps the
         * command in progress -- only the stale command-phase MISO bytes go.
         */
        qup_fifo_flush(q);
        if (!q->cs) {
            qup_bus_reset(q);
        }
    }
    if (off >= QUP_OUT_FIFO && off < QUP_OUT_FIFO + 0x40) {
        /*
         * Byte packing: the driver does `ldrb r3,[buf],#1; str r3,[fifo]`, so
         * a 32-bit store carries exactly one SPI byte and the read side pops
         * one byte per load.  Do not unpack size bytes here -- that inserted
         * three phantom zeros after every real byte.
         */
        qup_clock(q, val & 0xff);
        return;
    }
    if (idx < ARRAY_SIZE(q->shadow)) {
        q->shadow[idx] = val;
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

    /* QUP configuration registers come up cleared, not "nothing here" */
    memory_region_init_io(&q->mr, NULL, &qupl_ops, q, name, size);
    memory_region_add_subregion(parent, base, &q->mr);
    qup_spi = q;
    return q;
}

/* ------------------------------------------------------------------ */
/* BLSP1 BAM: the DMA engine that feeds the QUP FIFOs                  */
/* ------------------------------------------------------------------ */

/*
 * The QUP's FIFOs have two masters.  Software can drive them itself with
 * plain loads and stores (the "PIO" path above), or it can hand the job to
 * the BLSP BAM, which walks a ring of descriptors out of DRAM and does the
 * FIFO accesses on its own.  QCA's boot chain uses the BAM, and different
 * SBL1 builds use it in different ways, so the machine models the BAM
 * itself rather than patching any one build's decision.
 *
 * Register map, as both boot chains program it (n = pipe number, 0..15):
 *
 *   0x13000 + n*0x1000 + 0x000  PIPE_CNFG    bit1 = enable handshake,
 *                                            bit3 = consumer (QUP -> DRAM)
 *                       0x004    PIPE_ENABLE
 *                       0x028    PIPE_EVENT   bit17 idle, bit20 complete
 *                       0x800    ring consumer offset (bytes)
 *                       0x818    ring producer offset (bytes) -- the doorbell
 *                       0x81c    ring base address
 *                       0x820    ring size in bytes (low 16 bits)
 *                       0x828    ring limit (unused here)
 *
 * A ring entry is 8 bytes, { u32 addr; u32 ctrl; }, with the transfer length
 * in ctrl[19:0].  A producer pipe copies DRAM -> QUP output FIFO, a consumer
 * pipe copies QUP input FIFO -> DRAM, and the ring offsets wrap at ring size.
 *
 * The FIFO side is the same byte-at-a-time datapath the CPU path uses, so
 * whatever the die model sees here is identical to the PIO case.
 */

#define BAM_PIPE_FIRST      0x13000u
#define BAM_PIPE_STRIDE     0x1000u
#define BAM_PIPE_COUNT      16

/*
 * Per-pipe registers, offset relative to BAM_PIPE_FIRST + n * BAM_PIPE_STRIDE.
 * Names follow arch-qcom-common/bam.h (BAM v1.7), not the old guesses.
 */
#define BAM_P_CTRL           0x000u  /* BAM_P_CTRLn          */
#define BAM_P_RST            0x004u  /* BAM_P_RSTn           */
#define BAM_P_IRQ_STTS       0x010u  /* BAM_P_IRQ_STTSn      */
#define BAM_P_IRQ_CLR        0x014u  /* BAM_P_IRQ_CLRn       */
#define BAM_P_IRQ_EN         0x018u  /* BAM_P_IRQ_ENn        */
#define BAM_P_EVENT          0x028u  /* event register (polled) */
#define BAM_P_SW_OFSTS       0x800u  /* BAM_P_SW_OFSTSn      */
#define BAM_P_EVNT_REG       0x818u  /* BAM_P_EVNT_REGn      */
#define BAM_P_DESC_FIFO_ADDR 0x81cu  /* BAM_P_DESC_FIFO_ADDRn */
#define BAM_P_FIFO_SIZES     0x820u  /* BAM_P_FIFO_SIZESn    */

#define BAM_DESC_SIZE       8u
#define BAM_DESC_LEN_MASK   0x000fffffu

/* BAM_P_CTRLn bits */
#define BAM_CTRL_ENABLE      (1u << 1)  /* P_ENABLE */
#define BAM_CTRL_DIR_BAM2SYS (1u << 3)  /* 1: BAM -> system (QUP to DRAM)
                                         * 0: system -> BAM (DRAM to QUP) */

/* BAM_P_IRQ_*n bits, the "p_int_type" enum in bam.h */
#define BAM_P_IRQ_PRCSD_DESC  (1u << 0) /* P_PRCSD_DESC_EN_MASK */
#define BAM_P_IRQ_OUT_OF_DESC (1u << 3) /* P_OUT_OF_DESC_EN_MASK */
#define BAM_P_IRQ_ERR         (1u << 4) /* P_ERR_EN_MASK */
#define BAM_P_IRQ_TRNSFR_END  (1u << 5) /* P_TRNSFR_END_EN_MASK */

/*
 * Per-EE interrupt block: BAM_IRQ_SRCS/..._MSK at 0x3000 + 0x1000 * ee.
 * One bit per pipe, and the sources bit is the AND of the pipe status with
 * the pipe interrupt enable -- so a polled pipe (IRQ_EN left at 0, as SBL1
 * does) never shows up here, exactly like the hardware.
 */
#define BAM_IRQ_EE0          0x3000u
#define BAM_IRQ_EE_STRIDE    0x1000u
#define BAM_IRQ_SRCS         BAM_IRQ_EE0
#define BAM_IRQ_SRCS_MSK     (BAM_IRQ_EE0 + 0x4)
#define BAM_IRQ_SRCS_PIPE_MASK 0x7fffu

/* PIPE_EVENT bits.  Bit 20 is the transfer-complete flag the boot chain waits
 * for; bit 17 is a busy flag and has to stay clear once the ring is drained,
 * because the driver spins while it is set. */
#define BAM_EV_PIPE_BUSY    0x00020000u
#define BAM_EV_COMPLETE     0x00100000u

typedef struct IPQBamPipe {
    uint32_t ctrl;
    uint32_t event;
    uint32_t irq_stts;          /* BAM_P_IRQ_STTSn */
    uint32_t irq_en;            /* BAM_P_IRQ_ENn   */
    uint32_t limit;
    uint32_t ring_base;
    uint32_t ring_size;
    uint32_t prod;
    uint32_t cons;
    uint64_t bytes;             /* total moved, for the SPI log */
} IPQBamPipe;

typedef struct IPQBam {
    MemoryRegion mr;
    uint32_t *shadow;           /* everything not explicitly modelled */
    uint8_t *valid;
    uint32_t nregs;
    uint32_t irq_srcs_msk;      /* BAM_IRQ_SRCS_MSK of EE0 */
    IPQBamPipe pipe[BAM_PIPE_COUNT];
} IPQBam;

/* BAM-side FIFO access: the same datapath the CPU uses, one byte per access */
static void qup_bam_tx(IPQQupl *q, uint8_t b)
{
    qup_clock(q, b);
}

static uint8_t qup_bam_rx(IPQQupl *q)
{
    /*
     * A receive pipe drains the input FIFO.  When the QUP has not been clocked
     * for those bytes yet -- a pure read phase puts no DRAM bytes on MOSI at
     * all -- the hardware keeps clocking with a dummy byte, which is why a
     * 1-byte command can return a 5-byte reply.  Do the same.
     */
    if (q->resp_len == 0) {
        qup_clock(q, 0x00);
    }
    return (uint8_t)qup_in_word(q);
}

static bool bam_pipe_lookup(hwaddr off, int *n, hwaddr *roff)
{
    if (off < BAM_PIPE_FIRST || off >= IPQ_BLSP1_BAM_SIZE) {
        return false;
    }
    *n = (off - BAM_PIPE_FIRST) / BAM_PIPE_STRIDE;
    *roff = (off - BAM_PIPE_FIRST) % BAM_PIPE_STRIDE;
    return *n < BAM_PIPE_COUNT;
}

static void bam_pipe_xfer(IPQBam *b, int n, uint32_t addr, uint32_t len)
{
    IPQBamPipe *p = &b->pipe[n];
    uint32_t done, i;
    uint8_t buf[256];

    if (!qup_spi) {
        return;
    }
    if (p->ctrl & BAM_CTRL_DIR_BAM2SYS) {
        for (i = 0; i < len; i++) {
            uint8_t v = qup_bam_rx(qup_spi);

            address_space_write(&address_space_memory, addr + i,
                                MEMTXATTRS_UNSPECIFIED, &v, 1);
        }
    } else {
        for (done = 0; done < len; done += i) {
            i = MIN(len - done, (uint32_t)sizeof(buf));
            address_space_read(&address_space_memory, addr + done,
                               MEMTXATTRS_UNSPECIFIED, buf, i);
            for (uint32_t j = 0; j < i; j++) {
                qup_bam_tx(qup_spi, buf[j]);
            }
        }
    }
    p->bytes += len;
}

/*
 * Walk everything the doorbell just published and move the bytes.
 *
 * A processed descriptor raises BAM_P_IRQ_PRCSD_DESC and TRNSFR_END in the
 * pipe status word.  The bits stay pending until software writes
 * BAM_P_IRQ_CLRn (write-one-to-clear), which is what both U-Boot's
 * bam_wait_for_interrupt() and SBL1's flash driver rely on.
 */
static void bam_pipe_run(IPQBam *b, int n)
{
    IPQBamPipe *p = &b->pipe[n];
    int ndesc = 0;
    int guard = 0;

    if (!p->ring_base || !p->ring_size) {
        return;
    }
    while (p->cons != p->prod && guard++ < 4096) {
        uint32_t d[2];
        uint32_t addr, ctrl, len;

        if (p->cons + BAM_DESC_SIZE > p->ring_size) {
            p->cons = 0;
            continue;
        }
        address_space_read(&address_space_memory, p->ring_base + p->cons,
                           MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
        addr = d[0];
        ctrl = d[1];
        len = ctrl & BAM_DESC_LEN_MASK;
        p->cons += BAM_DESC_SIZE;
        if (p->cons >= p->ring_size) {
            p->cons = 0;
        }
        ndesc++;
        if (len == 0) {
            continue;
        }
        bam_pipe_xfer(b, n, addr, len);
        if (env_true("QEMU_IPQ_SPI_LOG")) {
            qemu_log_mask(LOG_UNIMP, "bam-xfer pipe %d %s 0x%08x %u bytes\n",
                          n, (p->ctrl & BAM_CTRL_DIR_BAM2SYS) ?
                          "qup->ram" : "ram->qup", addr, len);
        }
    }
    if (!ndesc) {
        return;
    }
    p->event |= env_u32("QEMU_IPQ_BAM_EVENT", BAM_EV_COMPLETE);
    p->irq_stts |= BAM_P_IRQ_PRCSD_DESC | BAM_P_IRQ_TRNSFR_END;
}

/* Per-pipe interrupt sources, as EE0 sees them (BAM_IRQ_SRCS). */
static uint32_t bam_irq_srcs(IPQBam *b)
{
    uint32_t v = 0;
    int i;

    for (i = 0; i < BAM_PIPE_COUNT; i++) {
        if (b->pipe[i].irq_stts & b->pipe[i].irq_en) {
            v |= 1u << i;
        }
    }
    return v;
}

/*
 * Unimplemented BAM offsets.  A real BAM register file reads 0 for reserved or
 * idle registers, but SBL1's flash init probes some of them and only accepts
 * ~0 as "nothing there", so keep that as the default and model the few
 * registers that actually matter individually.
 */
static uint32_t bam_def_read(void)
{
    return env_u32("QEMU_IPQ_BAM_DEF", 0xffffffffu);
}

static uint64_t bam_read(void *opaque, hwaddr off, unsigned size)
{
    IPQBam *b = opaque;
    uint32_t idx = off >> 2;
    hwaddr roff;
    int n;
    uint32_t v;

    if (off == BAM_IRQ_SRCS) {
        v = bam_irq_srcs(b) & BAM_IRQ_SRCS_PIPE_MASK;
        goto trace;
    }
    if (off == BAM_IRQ_SRCS_MSK) {
        v = b->irq_srcs_msk;
        goto trace;
    }

    if (bam_pipe_lookup(off, &n, &roff)) {
        IPQBamPipe *p = &b->pipe[n];

        switch (roff) {
        case BAM_P_CTRL:            v = p->ctrl; break;
        case BAM_P_RST:             v = 0; break;
        case BAM_P_IRQ_STTS:        v = p->irq_stts; break;
        case BAM_P_IRQ_EN:          v = p->irq_en; break;
        case BAM_P_EVENT:           v = p->event; break;
        case BAM_P_SW_OFSTS:        v = p->cons; break;
        case BAM_P_EVNT_REG:        v = p->prod; break;
        case BAM_P_DESC_FIFO_ADDR:  v = p->ring_base; break;
        case BAM_P_FIFO_SIZES:      v = p->ring_size; break;
        default:
            v = idx < b->nregs && b->valid[idx] ?
                b->shadow[idx] & 0x7ffffffeu : bam_def_read();
            break;
        }
    } else if (idx < b->nregs && b->valid[idx]) {
        v = b->shadow[idx] & 0x7ffffffeu;
    } else {
        v = bam_def_read();
    }

trace:
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "blsp1-bam: read  off 0x%03" HWADDR_PRIx
                      " -> 0x%08x%s\n", off, v,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    return v;
}

static void bam_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQBam *b = opaque;
    uint32_t idx = off >> 2;
    hwaddr roff;
    int n;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "blsp1-bam: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", off, val,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    if (idx < b->nregs) {
        b->shadow[idx] = val;
        b->valid[idx] = 1;
    }
    if (off == BAM_IRQ_SRCS_MSK) {
        b->irq_srcs_msk = val;
        return;
    }
    if (!bam_pipe_lookup(off, &n, &roff)) {
        return;
    }
    switch (roff) {
    case BAM_P_CTRL:
        b->pipe[n].ctrl = val;
        break;
    case BAM_P_RST:
        /* Software reset of the pipe state machine. */
        if (val & 1) {
            b->pipe[n].ctrl = 0;
            b->pipe[n].event = 0;
            b->pipe[n].irq_stts = 0;
            b->pipe[n].prod = 0;
            b->pipe[n].cons = 0;
        }
        break;
    case BAM_P_IRQ_CLR:
        b->pipe[n].irq_stts &= ~(uint32_t)val;   /* write-one-to-clear */
        break;
    case BAM_P_IRQ_EN:
        b->pipe[n].irq_en = val;
        break;
    case BAM_P_EVENT:
        b->pipe[n].event &= val;                /* write-one-to-clear */
        break;
    case BAM_P_DESC_FIFO_ADDR:
        b->pipe[n].ring_base = val;
        break;
    case BAM_P_FIFO_SIZES:
        b->pipe[n].ring_size = val & 0xffffu;
        break;
    case BAM_P_EVNT_REG:
        b->pipe[n].prod = val & 0xffffu;
        bam_pipe_run(b, n);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bam_ops = {
    .read = bam_read,
    .write = bam_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static IPQBam *add_bam(MemoryRegion *parent, hwaddr base, hwaddr size,
                       const char *name)
{
    IPQBam *b = g_new0(IPQBam, 1);

    b->nregs = size >> 2;
    b->shadow = g_new0(uint32_t, b->nregs);
    b->valid = g_new0(uint8_t, b->nregs);
    memory_region_init_io(&b->mr, NULL, &bam_ops, b, name, size);
    memory_region_add_subregion(parent, base, &b->mr);
    return b;
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
typedef struct IPQRegFile {
    MemoryRegion mr;
    uint32_t *regs;
    uint32_t nregs;
    unsigned dbg;                   /* log writes when QEMU_IPQ_ESS_LOG is set */
} IPQRegFile;

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

static IPQRegFile *add_regfile(MemoryRegion *parent, hwaddr base, hwaddr size,
                               const char *name)
{
    IPQRegFile *r = g_new0(IPQRegFile, 1);

    r->nregs = size / 4;
    r->regs = g_new0(uint32_t, r->nregs);
    r->dbg = getenv("QEMU_IPQ_ESS_LOG") != NULL;
    memory_region_init_io(&r->mr, NULL, &regfile_ops, r, name, size);
    memory_region_add_subregion(parent, base, &r->mr);
    return r;
}

/* ------------------------------------------------------------------ */
/* Ethernet: QCA MDIO master + clause-22 PHYs                          */
/* ------------------------------------------------------------------ */

/*
 * IPQ40xx exposes the MDIO master of the built-in switch at 0x00090000.  The
 * boot chain (and Linux) drive it through four registers, so this is modelled
 * register-for-register rather than stubbed:
 *
 *   0x40  MDIO_MODE       clock divider, read/write latch
 *   0x44  MDIO_ADDR       { phy[15:8], reg[4:0] }
 *   0x48  MDIO_DATA_WR   write data (clause 22)
 *   0x4c  MDIO_DATA_RD   read data
 *   0x50  MDIO_CMD        bit16 ACCESS_START/busy, bit0 = write access code
 *
 * An access completes immediately here (no MDIO clock is simulated), so
 * ACCESS_START reads back clear and the driver's busy poll falls through.
 *
 * Which PHYs sit on the bus is board data: it comes from the board device
 * tree (mdio node children) or from QEMU_IPQ_ETH_PORTS.  Ports are given as a
 * comma-separated addr[:speed[:id]] list, e.g.
 *
 *   QEMU_IPQ_ETH_PORTS="4:1000,5:100:0x004dd072"
 *
 * and a leading '!' marks a port whose link is down:
 *
 *   QEMU_IPQ_ETH_PORTS="0:1000,1:!1000"
 */
#define IPQ_MDIO_BASE        0x00090000u
#define IPQ_MDIO_SIZE        0x00001000u
#define IPQ_MDIO_MAX_PHYS    8
/*
 * A QCA8075 exposes its five copper PHYs at MDIO addresses 0..4 and its
 * PSGMII control block at address 5 (U-Boot's PSGMII_ID), so that is the
 * default population of the bus.  Boards with a different switch or fewer
 * ports override this.
 */
#define IPQ_ETH_DEFAULT_PORTS "0,1,2,3,4,5"
#define IPQ_ETH_DEFAULT_PHY_ID 0x004dd0b1u     /* QCA8075 */

#define MDIO_MODE_REG   0x40
#define MDIO_ADDR_REG   0x44
#define MDIO_WDATA_REG  0x48
#define MDIO_RDATA_REG  0x4c
#define MDIO_CMD_REG    0x50
#define MDIO_CMD_START  0x00000100u     /* bit 8: access start (write) */
#define MDIO_CMD_BUSY   0x00010000u     /* bit 16: busy (read-only) */
#define MDIO_CMD_WRITE  0x00000001u

/* clause 22 registers this model answers explicitly */
#define MII_BMCR        0
#define MII_BMSR        1
#define MII_PHYSID1     2
#define MII_PHYSID2     3
#define MII_ADVERTISE   4
#define MII_LPA         5
#define MII_STAT1000    10
#define MII_PHYSTS      0x11    /* vendor: Atheros/QCA status register */

#define PHY_MMD_SLOTS 32

typedef struct IPQPhy {
    int present;
    int addr;
    int link;
    int speed;                  /* Mbit/s */
    int full_duplex;
    uint32_t id;
    uint16_t reg[32];           /* everything else latches */

    /*
     * Clause 22 indirect registers: reg 0x0d selects the MMD device and
     * switches between "address" and "data" mode (bit 14 = no post
     * increment), reg 0x0e carries the register number or the data.  The
     * space is sparse, so keep a tiny associative map instead of 8 x 64 KiB.
     */
    uint16_t mmd_dev;
    uint16_t mmd_reg;
    int mmd_data_mode;
    uint32_t mmd_key[PHY_MMD_SLOTS];
    uint16_t mmd_val[PHY_MMD_SLOTS];
    int nmmd;
} IPQPhy;

typedef struct IPQMdio {
    MemoryRegion mr;
    uint32_t mode;
    uint32_t addr;
    uint32_t wdata;
    uint32_t rdata;
    uint32_t cmd;
    int nphy;
    IPQPhy phy[IPQ_MDIO_MAX_PHYS];
} IPQMdio;

static int mmd_find(IPQPhy *p, uint32_t key)
{
    int i;

    for (i = 0; i < p->nmmd; i++) {
        if (p->mmd_key[i] == key) {
            return i;
        }
    }
    return -1;
}

static uint16_t mmd_get(IPQPhy *p, uint16_t dev, uint16_t addr)
{
    int i = mmd_find(p, ((uint32_t)dev << 16) | addr);

    return i < 0 ? 0 : p->mmd_val[i];
}

static void mmd_set(IPQPhy *p, uint16_t dev, uint16_t addr, uint16_t val)
{
    uint32_t key = ((uint32_t)dev << 16) | addr;
    int i = mmd_find(p, key);

    if (i < 0) {
        if (p->nmmd >= PHY_MMD_SLOTS) {
            return;
        }
        i = p->nmmd++;
        p->mmd_key[i] = key;
    }
    p->mmd_val[i] = val;
}

/*
 * The PHY's built-in packet generator/checker (MMD7 registers 0x8020..0x802f)
 * counts the frames it sends through the internal loopback.  A healthy PHY
 * with the MAC in loopback sees 0x3000 good frames and no errors, which is the
 * result the boot chain's PSGMII self test looks for.  Traffic is started by
 * writing 0xa000 to 0x8020 and stopped by writing 0.
 */
static void phy_mmd_start_traffic(IPQPhy *p, uint16_t val)
{
    if (val == 0xa000) {
        mmd_set(p, 7, 0x802e, 0x3000);   /* tx ok, low 16 bits */
        mmd_set(p, 7, 0x802d, 0x0000);   /* tx ok, high 16 bits */
        mmd_set(p, 7, 0x802f, 0x0000);   /* tx errors */
        mmd_set(p, 7, 0x802b, 0x3000);   /* rx ok, low 16 bits */
        mmd_set(p, 7, 0x802a, 0x0000);   /* rx ok, high 16 bits */
        mmd_set(p, 7, 0x802c, 0x0000);   /* rx errors */
    } else if (val == 0) {
        mmd_set(p, 7, 0x802e, 0);
        mmd_set(p, 7, 0x802d, 0);
        mmd_set(p, 7, 0x802f, 0);
        mmd_set(p, 7, 0x802b, 0);
        mmd_set(p, 7, 0x802a, 0);
        mmd_set(p, 7, 0x802c, 0);
    }
}

static void phy_reset_regs(IPQPhy *p)
{
    memset(p->reg, 0, sizeof(p->reg));
    p->reg[MII_BMCR] = 0x1140;          /* AN enable, 1000 Mb/s, full duplex */
    p->reg[MII_PHYSID1] = p->id >> 16;
    p->reg[MII_PHYSID2] = p->id & 0xffff;
    p->reg[MII_ADVERTISE] = 0x01e1;     /* 10/100, FD + HD */
    p->reg[MII_LPA] = 0x45e1;           /* link partner says the same */
    p->reg[9] = 0x0300;                 /* 1000BASE-T control: FD + HD */
    p->reg[15] = 0x3000;                /* extended status: 1000BASE-T */
    /*
     * MMD1 register 0x28 bit 0 is the PSGMII PLL/VCO calibration-done flag.
     * The chip reports it set once the link is calibrated; a model that
     * returned 0 would make every boot wait out the 100 x 10 ms poll.
     */
    if (p->id == IPQ_ETH_DEFAULT_PHY_ID) {
        mmd_set(p, 1, 0x28, 0x0001);
    }
}

static uint16_t phy_read_reg(IPQPhy *p, int reg)
{
    if (reg == 0x0e && p->mmd_data_mode) {
        return mmd_get(p, p->mmd_dev, p->mmd_reg);
    }
    switch (reg) {
    case MII_BMSR:
        /*
         * Capabilities are static, status is live: the link and the completed
         * autonegotiation are what the firmware polls for.
         */
        return 0x7949 | (p->link ? 0x0004 : 0) | (p->link ? 0x0020 : 0);
    case MII_STAT1000:
        return p->link && p->speed == 1000 ? 0x3800 : 0x0000;
    case MII_PHYSTS:
        /*
         * The Atheros/QCA vendor status register, which is what the boot
         * chain polls when it waits for a port to come up:
         *
         *   bit 10    link up
         *   bit 13    full duplex
         *   bit 15:14 resolved speed (0 = 10, 1 = 100, 2 = 1000 Mb/s)
         *
         * Nothing else is reported: the MDI crossover and autoneg-complete
         * bits are not needed by any driver here.
         */
        if (!p->link) {
            return p->reg[MII_PHYSTS];
        }
        return 0x0400 |
               (p->full_duplex ? 0x2000 : 0) |
               ((p->speed >= 1000 ? 2 : p->speed >= 100 ? 1 : 0) << 14);
    default:
        return p->reg[reg & 0x1f];
    }
}

static void phy_write_reg(IPQPhy *p, int reg, uint16_t val)
{
    if (reg == 0x0d) {
        p->mmd_dev = val & 0x1f;
        p->mmd_data_mode = (val & 0x4000) != 0;
        return;
    }
    if (reg == 0x0e) {
        if (p->mmd_data_mode) {
            mmd_set(p, p->mmd_dev, p->mmd_reg, val);
            if (p->mmd_dev == 7) {
                phy_mmd_start_traffic(p, val);
            }
        } else {
            p->mmd_reg = val;
        }
        return;
    }
    if (reg == MII_BMCR && (val & 0x8000)) {
        phy_reset_regs(p);              /* BMCR reset is self-clearing */
        return;
    }
    if (reg == MII_BMCR) {
        p->speed = (val & 0x2000) ? 1000 : (val & 0x0040) ? 100 : 10;
        p->full_duplex = (val & 0x0100) != 0;
    }
    p->reg[reg & 0x1f] = val;
}

static IPQPhy *mdio_lookup(IPQMdio *m, int addr)
{
    int i;

    for (i = 0; i < m->nphy; i++) {
        if (m->phy[i].present && m->phy[i].addr == addr) {
            return &m->phy[i];
        }
    }
    return NULL;
}

static uint64_t mdio_read(void *opaque, hwaddr off, unsigned size)
{
    IPQMdio *m = opaque;
    uint32_t v;

    switch (off) {
    case MDIO_MODE_REG:  v = m->mode; break;
    case MDIO_ADDR_REG:  v = m->addr; break;
    case MDIO_WDATA_REG: v = m->wdata; break;
    case MDIO_RDATA_REG: v = m->rdata; break;
    case MDIO_CMD_REG:   v = m->cmd; break;
    default:             v = 0; break;
    }
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "mdio: read  off 0x%02" HWADDR_PRIx
                      " -> 0x%08x%s\n", off, v,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    return v;
}

static void mdio_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQMdio *m = opaque;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "mdio: write off 0x%02" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", off, val,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    switch (off) {
    case MDIO_MODE_REG:
        m->mode = val;
        break;
    case MDIO_ADDR_REG:
        m->addr = val;
        break;
    case MDIO_WDATA_REG:
        m->wdata = val;
        break;
    case MDIO_CMD_REG:
        if (val & MDIO_CMD_START) {
            int phy_addr = (m->addr >> 8) & 0x1f;
            int reg = m->addr & 0x1f;
            IPQPhy *p = mdio_lookup(m, phy_addr);
            int i;

            /*
             * Clause 22 broadcast address: the QCA8075 is wired so that a
             * write to PHY 31 reaches every port (that is how the self test
             * configures all five at once).  Only writes make sense there.
             */
            if (phy_addr == 0x1f && (val & MDIO_CMD_WRITE)) {
                for (i = 0; i < m->nphy; i++) {
                    if (m->phy[i].present) {
                        phy_write_reg(&m->phy[i], reg, m->wdata & 0xffff);
                    }
                }
                m->cmd = 0;
                break;
            }

            if (!p) {
                /* Nothing on the bus: MDIO reads float high. */
                m->rdata = 0xffff;
            } else if (val & MDIO_CMD_WRITE) {
                phy_write_reg(p, reg, m->wdata & 0xffff);
            } else {
                m->rdata = phy_read_reg(p, reg);
            }
            if (env_true("QEMU_IPQ_TRACE")) {
                qemu_log_mask(LOG_UNIMP, "mdio: %s phy %d reg %02x %s 0x%04x%s\n",
                              (val & MDIO_CMD_WRITE) ? "wr" : "rd", phy_addr, reg,
                              (val & MDIO_CMD_WRITE) ? "<-" : "->",
                              (val & MDIO_CMD_WRITE) ? (m->wdata & 0xffff) : m->rdata,
                              env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
            }
        }
        m->cmd = 0;                     /* the access is already finished */
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mdio_ops = {
    .read = mdio_read,
    .write = mdio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mdio_add_phy(IPQMdio *m, int addr, int speed, uint32_t id, int link)
{
    IPQPhy *p;

    if (m->nphy >= IPQ_MDIO_MAX_PHYS) {
        warn_report("ipq4019: too many Ethernet ports, ignoring phy %d", addr);
        return;
    }
    p = &m->phy[m->nphy++];
    p->present = 1;
    p->addr = addr;
    p->speed = speed;
    p->link = link;
    p->full_duplex = 1;
    p->id = id;
    phy_reset_regs(p);
}

/*
 * Parse "addr[:speed[:id]]" entries separated by commas.  A leading '!' on the
 * port means "link down"; that is how a board with an empty SFP cage or a
 * switch port with nothing plugged in looks.
 */
static void mdio_parse_ports(IPQMdio *m, const char *spec)
{
    uint32_t def_id = env_u32("QEMU_IPQ_ETH_PHY_ID", IPQ_ETH_DEFAULT_PHY_ID);

    while (spec && *spec) {
        char *end;
        long addr, speed = 1000, link = 1;
        uint32_t id = def_id;

        while (*spec == ',' || *spec == ' ') {
            spec++;
        }
        if (!*spec) {
            break;
        }
        if (*spec == '!') {
            link = 0;
            spec++;
        }
        addr = strtol(spec, &end, 0);
        if (end == spec || addr < 0 || addr > 31) {
            warn_report("ipq4019: bad QEMU_IPQ_ETH_PORTS entry at '%s'", spec);
            break;
        }
        spec = end;
        if (*spec == ':') {
            speed = strtol(spec + 1, &end, 0);
            spec = end;
        }
        if (*spec == ':') {
            id = strtoul(spec + 1, &end, 0);
            spec = end;
        }
        mdio_add_phy(m, addr, speed, id, link);
    }
}

/* ------------------------------------------------------------------ */
/* Board ports from a device tree                                      */
/* ------------------------------------------------------------------ */

/*
 * The PHYs on the MDIO bus are board data, and boards describe them in their
 * device tree:
 *
 *     mdio@90000 {
 *         compatible = "qcom,ipq4019-mdio";
 *         ethernet-phy-package@0 {
 *             ethernet-phy@0 { reg = <0>; };
 *             ...
 *             psgmii-phy@5  { reg = <5>; };
 *         };
 *     };
 *
 * so a DTB handed to the machine with -dtb (or QEMU_IPQ_DTB=...) is enough to
 * populate the bus exactly as the board does.  The firmware still boots its
 * own DTB from flash; the one given here only describes the hardware to the
 * machine model, which is why it is consumed instead of being loaded into
 * guest RAM.
 */
static int dtb_collect_phys(void *fdt, int node, IPQMdio *m, int *count)
{
    int sub;

    for (sub = fdt_first_subnode(fdt, node); sub >= 0;
         sub = fdt_next_subnode(fdt, sub)) {
        const char *name = fdt_get_name(fdt, sub, NULL);
        const fdt32_t *reg;
        int len;

        if (!name) {
            continue;
        }
        if (strncmp(name, "ethernet-phy@", 13) != 0 &&
            strncmp(name, "psgmii-phy@", 11) != 0 &&
            strncmp(name, "phy@", 4) != 0) {
            /* e.g. ethernet-phy-package@0: keep looking inside */
            dtb_collect_phys(fdt, sub, m, count);
            continue;
        }
        reg = fdt_getprop(fdt, sub, "reg", &len);
        if (!reg || len < 4) {
            continue;
        }
        mdio_add_phy(m, fdt32_to_cpu(reg[0]),
                     env_u32("QEMU_IPQ_ETH_SPEED", 1000),
                     env_u32("QEMU_IPQ_ETH_PHY_ID", IPQ_ETH_DEFAULT_PHY_ID),
                     1);
        (*count)++;
    }
    return *count;
}

static int dtb_find_ports(void *fdt, int node)
{
    int sub;

    for (sub = fdt_first_subnode(fdt, node); sub >= 0;
         sub = fdt_next_subnode(fdt, sub)) {
        const char *name = fdt_get_name(fdt, sub, NULL);
        int found;

        if (name && !strcmp(name, "ports")) {
            return sub;
        }
        found = dtb_find_ports(fdt, sub);
        if (found >= 0) {
            return found;
        }
    }
    return -1;
}

/*
 * Print the switch port map, including which PHY each port is wired to.  This
 * is the "attach the ports the way the DTS says" evidence in the boot log; the
 * packet path itself is not modelled yet, so nothing here affects booting.
 */
static void dtb_report_switch(void *fdt)
{
    int ports = dtb_find_ports(fdt, 0);
    int port;

    if (ports < 0) {
        return;
    }
    for (port = fdt_first_subnode(fdt, ports); port >= 0;
         port = fdt_next_subnode(fdt, port)) {
        const char *name = fdt_get_name(fdt, port, NULL);
        const fdt32_t *reg, *ph;
        const char *label;
        int len, phy = -1, phy_node;

        if (!name || strncmp(name, "port@", 5) != 0) {
            continue;
        }
        reg = fdt_getprop(fdt, port, "reg", &len);
        label = fdt_getprop(fdt, port, "label", &len);
        ph = fdt_getprop(fdt, port, "phy-handle", &len);
        if (ph && len >= 4) {
            phy_node = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(ph[0]));
            if (phy_node >= 0) {
                const fdt32_t *preg = fdt_getprop(fdt, phy_node, "reg", &len);

                if (preg && len >= 4) {
                    phy = fdt32_to_cpu(preg[0]);
                }
            }
        } else if (fdt_subnode_offset(fdt, port, "fixed-link") >= 0) {
            phy = -2;                   /* fixed link, no PHY */
        }
        printf("ipq4019: dtb port %d", reg ? (int)fdt32_to_cpu(reg[0]) : -1);
        if (label) {
            printf(" \"%s\"", label);
        }
        if (phy == -2) {
            printf(" fixed-link\n");
        } else if (phy >= 0) {
            printf(" -> mdio phy %d\n", phy);
        } else {
            printf(" -> disconnected\n");
        }
    }
}

static void mdio_add_from_dtb(IPQMdio *m, const char *path)
{
    void *fdt;
    int size = 0, mdio, count = 0;

    fdt = load_device_tree(path, &size);
    if (!fdt) {
        warn_report("ipq4019: cannot read device tree '%s', using defaults",
                    path);
        return;
    }
    mdio = fdt_node_offset_by_compatible(fdt, -1, "qcom,ipq4019-mdio");
    if (mdio < 0) {
        mdio = fdt_path_offset(fdt, "/soc/mdio@90000");
    }
    if (mdio < 0) {
        warn_report("ipq4019: no ipq4019 mdio node in '%s'", path);
        goto out;
    }
    dtb_collect_phys(fdt, mdio, m, &count);
    if (count) {
        printf("ipq4019: %d mdio port(s) from %s\n", count, path);
    }
    dtb_report_switch(fdt);
out:
    g_free(fdt);
}

/*
 * The board description may come from QEMU_IPQ_DTB (a DTB/DTS path, handy when
 * the firmware image is passed with -kernel) or from -dtb.
 */
static const char *ipq_machine_dtb(MachineState *machine)
{
    const char *path = getenv("QEMU_IPQ_DTB");

    if (!path && machine->dtb) {
        path = machine->dtb;
        /*
         * Consume it: this firmware has its own DTB in flash, and letting
         * arm_load_kernel() copy the description into guest RAM would land
         * on top of the image the boot loader is about to use.
         */
        g_free(machine->dtb);
        machine->dtb = NULL;
    }
    return path;
}

static IPQMdio *add_mdio(MemoryRegion *parent, const char *dtb_path)
{
    IPQMdio *m = g_new0(IPQMdio, 1);
    const char *spec = getenv("QEMU_IPQ_ETH_PORTS");
    int i;

    /*
     * Three sources, in order of precedence: an explicit port list, the board
     * device tree, the built-in QCA8075 population.
     */
    if (spec) {
        mdio_parse_ports(m, spec);
    } else if (dtb_path) {
        mdio_add_from_dtb(m, dtb_path);
        if (!m->nphy) {
            mdio_parse_ports(m, IPQ_ETH_DEFAULT_PORTS);
        }
    } else {
        mdio_parse_ports(m, IPQ_ETH_DEFAULT_PORTS);
    }

    memory_region_init_io(&m->mr, NULL, &mdio_ops, m, "mdio", IPQ_MDIO_SIZE);
    memory_region_add_subregion(parent, IPQ_MDIO_BASE, &m->mr);

    printf("ipq4019: mdio@%08x, %d port(s):", IPQ_MDIO_BASE, m->nphy);
    for (i = 0; i < m->nphy; i++) {
        printf(" %d@%d%s", m->phy[i].addr, m->phy[i].speed,
               m->phy[i].link ? "" : "(down)");
    }
    printf("\n");
    return m;
}

/* ------------------------------------------------------------------ */
/* BLSP UARTDM (TX only)                                               */
/* ------------------------------------------------------------------ */

typedef struct IPQUart {
    MemoryRegion mr;
    CharFrontend chr;
    const char *name;
} IPQUart;

static uint64_t uart_read(void *opaque, hwaddr off, unsigned size)
{
    IPQUart *u = opaque;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: read  off 0x%03" HWADDR_PRIx "%s\n",
                      u->name, off,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    /* A4 = SR (UARTDM), 08 = SR (legacy); both "ready" in all directions */
    if (off == 0xa4 || off == 0x08) {
        return env_u32("QEMU_IPQ_UART_SR", 0x0f);
    }
    /*
     * TX FIFO pointers.  SBL1 computes the free space as
     * (TCA & ~3) - ((TFWR >> 14) | (TFWR & 0x7f)); since the model drains the
     * FIFO immediately, report it permanently empty.
     */
    if (off == 0x44) {
        return env_u32("QEMU_IPQ_UART_FIFO", 0x100);
    }
    if (off == 0x4c) {
        return 0;
    }
    return 0;
}

static void uart_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQUart *u = opaque;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", u->name, off, val,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    /* TF (UARTDM) lives at 0x70..0x7c and 0x100, one byte per word */
    if ((off >= 0x70 && off < 0x80) || off == 0x100) {
        uint8_t c = val;

        qemu_chr_fe_write_all(&u->chr, &c, 1);
        return;
    }
    if (env_true("QEMU_IPQ_UART_LEGACY") && off < 4) {
        uint8_t c = val;

        qemu_chr_fe_write_all(&u->chr, &c, 1);
    }
}

static const MemoryRegionOps uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void add_uart(MemoryRegion *parent, hwaddr base, const char *name,
                     Chardev *chr)
{
    IPQUart *u = g_new0(IPQUart, 1);
    Error *err = NULL;

    u->name = name;
    if (!qemu_chr_fe_init(&u->chr, chr, &err)) {
        error_report_err(err);
        exit(1);
    }
    memory_region_init_io(&u->mr, NULL, &uart_ops, u, name, 0x1000);
    memory_region_add_subregion(parent, base, &u->mr);
}

/* ------------------------------------------------------------------ */
/* qca-gcnt: 32-bit free running counter                               */
/* ------------------------------------------------------------------ */

static uint64_t gcnt_read(void *opaque, hwaddr off, unsigned size)
{
    uint32_t hz = *(uint32_t *)opaque;
    uint64_t ticks;

    ticks = muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), hz,
                     NANOSECONDS_PER_SECOND);
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
static GArray *ipq_mapped;

static bool ipq_range_covered(hwaddr base, hwaddr size)
{
    int i;

    if (!ipq_mapped) {
        return false;
    }
    for (i = 0; i < ipq_mapped->len; i++) {
        IPQRange *r = &g_array_index(ipq_mapped, IPQRange, i);

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
    if (!ipq_mapped) {
        ipq_mapped = g_array_new(FALSE, FALSE, sizeof(IPQRange));
    }
    g_array_append_val(ipq_mapped, r);
}

/*
 * Load an ARM32 ELF boot stage (QCA SBL1) the way the PBL would: map RAM at
 * every PT_LOAD p_paddr and copy the segment there.  Returns false if the
 * file is not an ELF32/ARM image.
 */
static bool ipq4019_load_elf(MachineState *machine, MemoryRegion *sysmem)
{
    const char *path = machine->kernel_filename;
    gchar *buf;
    gsize len;
    Elf32_Ehdr *eh;
    int i;

    if (!path || !g_file_get_contents(path, &buf, &len, NULL)) {
        return false;
    }
    if (len < sizeof(Elf32_Ehdr) || memcmp(buf, ELFMAG, SELFMAG) != 0 ||
        buf[EI_CLASS] != ELFCLASS32 || buf[EI_DATA] != ELFDATA2LSB) {
        g_free(buf);
        return false;
    }
    eh = (Elf32_Ehdr *)buf;
    if (eh->e_machine != EM_ARM ||
        eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > len) {
        g_free(buf);
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
    printf("ipq4019: loaded %s (SBL ELF), entry 0x%08x, %d phdrs\n",
           path, ipq4019_elf_entry, eh->e_phnum);
    g_free(buf);
    return true;
}

static void ipq4019_reset(void *opaque)
{
    ARMCPU *cpu = opaque;
    CPUARMState *env = &cpu->env;
    uint32_t entry = ipq4019_elf_entry ?: env_u32("QEMU_IPQ_ENTRY", 0);

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

static void ipq4019_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rom;
    IPQMem *flash;
    IPQMem *bootctx;
    IPQMem *bootdev;
    IPQBam *bam;
    MemoryRegion *gcnt;
    MemoryRegion *qtimer;
    MemoryRegion *xo_timer;
    MemoryRegion *tsc_timer;
    uint32_t entry = env_u32("QEMU_IPQ_ENTRY", 0);
    int i;

    for (i = 0; i < machine->smp.cpus; i++) {
        cpu_create(machine->cpu_type);
    }

    memory_region_add_subregion(sysmem, IPQ_RAM_BASE, machine->ram);

    flash = add_mem(sysmem, IPQ_FLASH_BASE, IPQ_FLASH_SIZE, "flash");
    ipq_flash = flash;
    {
        const char *img = getenv("QEMU_IPQ_FLASH_IMAGE");
        const char *jstr = getenv("QEMU_IPQ_FLASH_JEDEC");
        gchar *buf = NULL;
        gsize len = 0;
        uint32_t die = env_u32("QEMU_IPQ_FLASH_SIZE", IPQ_FLASH_DEFAULT);
        unsigned int j;

        /*
         * Die geometry belongs to the board, not to the firmware image, so it
         * is configuration: QEMU_IPQ_FLASH_SIZE (bytes) and
         * QEMU_IPQ_FLASH_JEDEC (RDID bytes).  Defaults describe a 32 MiB
         * Winbond W25Q256, the part most ipq40xx boards ship with.
         */
        if (die < IPQ_FLASH_MIN || die > IPQ_FLASH_SIZE ||
            (die & (die - 1)) != 0) {
            error_report("ipq4019: QEMU_IPQ_FLASH_SIZE=%u must be a power of "
                         "two between %u and %u bytes",
                         die, (unsigned)IPQ_FLASH_MIN,
                         (unsigned)IPQ_FLASH_SIZE);
            exit(1);
        }
        ipq_flash_die_size = die;

        /* Default RDID: Winbond ef 40 <log2(bytes)>. */
        ipq_flash_jedec[0] = 0xef;
        ipq_flash_jedec[1] = 0x40;
        ipq_flash_jedec[2] = 31 - clz32(die);
        ipq_flash_jedec_len = 3;
        if (jstr && *jstr) {
            int n = 0;
            const char *p = jstr;

            while (*p && n < 4) {
                char *end;
                unsigned long v;

                while (*p && !g_ascii_isxdigit(*p)) {
                    p++;
                }
                if (!*p) {
                    break;
                }
                v = strtoul(p, &end, 16);
                ipq_flash_jedec[n++] = v & 0xff;
                p = end;
            }
            if (n < 2) {
                error_report("ipq4019: QEMU_IPQ_FLASH_JEDEC='%s' is not a "
                             "hex byte string like ef4019", jstr);
                exit(1);
            }
            ipq_flash_jedec_len = n;
        }

        /* Unwritten flash reads 0xff, not 0x00. */
        memset(flash->data, 0xff, flash->size);
        if (!img || !*img) {
            img = machine->kernel_filename;
        }
        if (img) {
            g_file_get_contents(img, &buf, &len, NULL);
        }
        if (buf) {
            memcpy(flash->data, buf, MIN(len, flash->size));
            g_free(buf);
        }
        printf("ipq4019: flash die %u MiB, RDID", (unsigned)(die / MiB));
        for (j = 0; j < ipq_flash_jedec_len; j++) {
            printf(" %02x", ipq_flash_jedec[j]);
        }
        printf(", window %u MiB <- %s (%zu bytes)\n",
               (unsigned)(IPQ_FLASH_SIZE / MiB), img ? img : "(none)", len);
    }
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
    add_stub(sysmem, IPQ_PSHOLD_BASE, 0x1000, "pshold");
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
    add_stub(sysmem, IPQ_TLMM_BASE, IPQ_TLMM_SIZE, "tlmm");
    /*
     * Clock blocks: bit 0 behaves as a self-clearing trigger on written
     * registers (software sets it, then polls until it reads 0).  Unwritten
     * registers keep reading 0xffffffff, which the PLL lock polls want.
     */
    add_stub_ex(sysmem, IPQ_GCC_BASE, IPQ_GCC_SIZE, "gcc", 0x7ffffffeu);
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
                                   "gcc", 0x7ffffffeu);

        gcc->def_read = 0;
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
     * The transport reads a "BAM version" field at +0x1000 and only accepts
     * 6..11, 19..25 or 32..39 (bam_get_version); the DTS describes bam-v1.7.0,
     * so seed 7.  Without it the field reads 0xff out of the untouched default
     * and the probe aborts with 0x303d.
     */
    bam = add_bam(sysmem, IPQ_BLSP1_BAM_BASE, IPQ_BLSP1_BAM_SIZE, "blsp1-bam");
    bam->shadow[0x1000 >> 2] = env_u32("QEMU_IPQ_BAM_VER", 7);
    bam->valid[0x1000 >> 2] = 1;
    add_qupl(sysmem, IPQ_QUP_BASE, 0x1000, "qup-spi");
    add_stub(sysmem, IPQ_QPIC_BASE, IPQ_QPIC_SIZE, "qpic-nand");
    /*
     * APCS clock/reset block: same self-clearing bit-0 trigger as the GCC
     * reset registers, polled by the shared clk_set_rate() helper.  Only the
     * low register page needs shadow storage.
     */
    add_stub_ex(sysmem, IPQ_QGIC_BASE, 0x20000, "apcs", 0x7ffffffeu);
    /*
     * The rest of the APCS range is a decode-only stub, minus the 0xb021000
     * page which carries the free-running XO counter (see xo-timer below).
     * Carve it out here so the two do not overlap.
     */
    add_stub(sysmem, IPQ_QGIC_BASE + 0x20000, 0x1000, "apcs-hi");
    add_stub(sysmem, IPQ_QGIC_BASE + 0x22000, 0xfe0000 - 0x2000, "apcs-hi2");
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
    /* Ethernet: MDIO master at 0x90000 (ports on it are board config). */
    add_mdio(sysmem, ipq_machine_dtb(machine));
    /*
     * ESS switch register block (ports, PSGMII, PCS) and the EDMA engine.
     * Both are register files for now: the boot chain programs them and
     * polls status bits, but no packet moves until EDMA descriptors are
     * modelled.
     */
    add_regfile(sysmem, 0x0c000000, 0x00080000, "ess");
    add_regfile(sysmem, 0x0c080000, 0x00080000, "edma");
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

    add_uart(sysmem, IPQ_UART0_BASE, "uart0", serial_hd(0));
    if (serial_hd(1)) {
        add_uart(sysmem, IPQ_UART1_BASE, "uart1", serial_hd(1));
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
        *(uint32_t *)(bi + 0x000) = env_u32("QEMU_IPQ_BOOTDEV_TYPE", 6);
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
        printf("ipq4019: boot-info handed to SBL1 in r0 = 0x%08x\n",
               ipq4019_bootinfo);
    } else {
        arm_load_kernel(ARM_CPU(first_cpu), machine, &ipq4019_binfo);
    }

    /*
     * Registered after arm_load_kernel() so that it runs after the CPUs'
     * own reset handlers, which clear CP15 state.
     */
    for (CPUState *cs = first_cpu; cs; cs = CPU_NEXT(cs)) {
        qemu_register_reset(ipq4019_reset, ARM_CPU(cs));
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
}

DEFINE_MACHINE_ARM("ipq4019", ipq4019_machine_init)
