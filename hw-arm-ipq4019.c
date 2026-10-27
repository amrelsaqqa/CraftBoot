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
 *   QEMU_IPQ_GCNT_HZ     qca-gcnt/uDelay tick rate (default 32768)
 *   QEMU_IPQ_NO_PMU_FIX=1  do not set PMCR.E at reset
 *   QEMU_IPQ_BOOTINFO    address of the synthesized PBL shared data handed to
 *                        SBL1 in r0 (default 0x08680000)
 *   QEMU_IPQ_BOOTCFG     value of the SoC boot-config word (0x5e02c, bits 9:0)
 *   QEMU_IPQ_DEVCFG      value of the boot-device descriptor words (0x5e030+)
 *   QEMU_IPQ_SPI_LOG=1   log the decoded SPI command stream (-d unimp)
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
#include "cpu.h"

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
#define IPQ_FLASH_SIZE    (64 * MiB)

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
#define QUP_REG_ERROR_FLAGS  0x01c
#define QUP_REG_SW_RESET     0x00c
#define QUP_OUT_FIFO         0x110
#define QUP_IN_FIFO          0x218

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
        /*
         * Winbond W25Q256: 32 MiB, matching the dump size.  The driver keys
         * its geometry (page/sector size, erase opcodes) off this.
         */
        q->rx_buf[0] = 0xef;
        q->rx_buf[1] = 0x40;
        q->rx_buf[2] = 0x19;
        q->rx_len = 3;
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
        return qup_flash_byte(q->rx_off++ & (IPQ_FLASH_SIZE - 1));
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
    q->resp_head = 0;
    q->resp_len = 0;
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
    if (off == QUP_REG_STATE && (val & 0x3) == 0) {
        /*
         * QCA's transport puts QUP through RESET before every transfer, which
         * is also where a real SPI die loses byte alignment: show it a fresh
         * opcode boundary.
         */
        qup_bus_reset(q);
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

#define BAM_P_CNFG          0x000u
#define BAM_P_ENABLE        0x004u
#define BAM_P_EVENT         0x028u
#define BAM_P_RING_CONS     0x800u
#define BAM_P_RING_PROD     0x818u
#define BAM_P_RING_BASE     0x81cu
#define BAM_P_RING_SIZE     0x820u
#define BAM_P_RING_LIMIT    0x828u

#define BAM_DESC_SIZE       8u
#define BAM_DESC_LEN_MASK   0x000fffffu

/* PIPE_CNFG bits this model interprets */
#define BAM_CNFG_CONSUMER   0x8u    /* pipe reads the QUP input FIFO */

/* PIPE_EVENT bits.  Bit 20 is the transfer-complete flag the boot chain waits
 * for; bit 17 is a busy flag and has to stay clear once the ring is drained,
 * because the driver spins while it is set. */
#define BAM_EV_PIPE_BUSY    0x00020000u
#define BAM_EV_COMPLETE     0x00100000u
typedef struct IPQBamPipe {
    uint32_t cnfg;
    uint32_t enable;
    uint32_t event;
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
    if (p->cnfg & BAM_CNFG_CONSUMER) {
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

/* Walk everything the doorbell just published and move the bytes. */
static void bam_pipe_run(IPQBam *b, int n)
{
    IPQBamPipe *p = &b->pipe[n];
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
        if (len == 0) {
            continue;
        }
        bam_pipe_xfer(b, n, addr, len);
        if (env_true("QEMU_IPQ_SPI_LOG")) {
            qemu_log_mask(LOG_UNIMP, "bam-xfer pipe %d %s 0x%08x %u bytes\n",
                          n, (p->cnfg & BAM_CNFG_CONSUMER) ?
                          "qup->ram" : "ram->qup", addr, len);
        }
    }
    p->event |= env_u32("QEMU_IPQ_BAM_EVENT", BAM_EV_COMPLETE);
}

static uint64_t bam_read(void *opaque, hwaddr off, unsigned size)
{
    IPQBam *b = opaque;
    uint32_t idx = off >> 2;
    hwaddr roff;
    int n;
    uint32_t v;

    if (bam_pipe_lookup(off, &n, &roff)) {
        IPQBamPipe *p = &b->pipe[n];

        switch (roff) {
        case BAM_P_CNFG:       v = p->cnfg; break;
        case BAM_P_ENABLE:     v = p->enable; break;
        case BAM_P_EVENT:      v = p->event; break;
        case BAM_P_RING_CONS:  v = p->cons; break;
        case BAM_P_RING_PROD:  v = p->prod; break;
        case BAM_P_RING_BASE:  v = p->ring_base; break;
        case BAM_P_RING_SIZE:  v = p->ring_size; break;
        case BAM_P_RING_LIMIT: v = p->limit; break;
        default:
            v = idx < b->nregs && b->valid[idx] ?
                b->shadow[idx] & 0x7ffffffeu : 0xffffffffu;
            break;
        }
    } else if (idx < b->nregs && b->valid[idx]) {
        v = b->shadow[idx] & 0x7ffffffeu;
    } else {
        v = 0xffffffffu;
    }
    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "blsp1-bam: read  off 0x%03" HWADDR_PRIx
                      " -> %s0x%08x%s\n", off,
                      (idx < b->nregs && b->valid[idx]) ? "" : "(default) ",
                      v, env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
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
    if (!bam_pipe_lookup(off, &n, &roff)) {
        return;
    }
    switch (roff) {
    case BAM_P_CNFG:
        b->pipe[n].cnfg = val;
        break;
    case BAM_P_ENABLE:
        b->pipe[n].enable = val & 1;
        break;
    case BAM_P_EVENT:
        b->pipe[n].event &= val;            /* write-one-to-clear */
        break;
    case BAM_P_RING_BASE:
        b->pipe[n].ring_base = val;
        break;
    case BAM_P_RING_SIZE:
        b->pipe[n].ring_size = val & 0xffffu;
        break;
    case BAM_P_RING_LIMIT:
        b->pipe[n].limit = val & 0xffffu;
        break;
    case BAM_P_RING_PROD:
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
    if (env_u32("QEMU_IPQ_TRACE", 0) > 1) {
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

        if (!img || !*img) {
            img = machine->kernel_filename;
        }
        if (img) {
            gchar *buf = NULL;
            gsize len = 0;

            if (g_file_get_contents(img, &buf, &len, NULL)) {
                memcpy(flash->data, buf, MIN(len, flash->size));
                printf("ipq4019: flash window <- %s (%zu bytes)\n",
                       img, MIN(len, flash->size));
                g_free(buf);
            }
        }
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
     * (low word at +0, high word at +4). Same time base as qca-gcnt.
     */
    qtimer = g_new0(MemoryRegion, 1);
    qtimer_hz = env_u32("QEMU_IPQ_GCNT_HZ", 32768);
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
    /* TCSR: SoC revision, boot option and strap registers */
    add_stub(sysmem, 0x1900000, 0x100000, "tcsr");
    /*
     * 0x08e00000..0x08e3ffff: qcrypto (0x08e20000) + crypto BAM (0x08e04000)
     * and the block SBL1 pokes during the post-CDT DDR/training step
     * (0x08e3a000..0x08e3dfff).  Decode-only for now; the bit-0 self-clearing
     * trigger mask keeps poll loops happy.
     */
    add_stub_ex(sysmem, 0x08e00000, 0x40000, "qcrypto", 0x7ffffffeu);

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
         * word, and boot_flash_target_init (boot_flash_target.c:0xa8c8034)
         * switches on it:
         *
         *   type 2      -> boot_flash_dev_nand_if init with a static table
         *   type 3,4,5  -> boot_flash_dev_nand_if init (the SPI NOR / SPI NAND
         *                  serial-flash transports)
         *   anything    -> boot_flash_init() reports "0 Bytes" and never
         *   else           probes the controller, so every later image load
         *                  fails and SBL1 falls into Sahara download mode.
         *
         * The reference device boots from SPI NOR, so 3 is the natural
         * default; sweep with QEMU_IPQ_BOOTDEV_TYPE while probing.
         *
         * Status: only 2 actually reaches the SPI NOR driver (types 3/4/5
         * trip a 0x300d assert in boot_flash_dev_nand_if, types 0/1 skip the
         * probe entirely).  Use QEMU_IPQ_BOOTDEV_TYPE=2 to engage the real
         * flash path; the default of 0 keeps the older "flash is never
         * probed" baseline so the boot log still reaches Image Load.
         */
        *(uint32_t *)(bi + 0x000) = env_u32("QEMU_IPQ_BOOTDEV_TYPE", 0);
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
