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
 *   QEMU_IPQ_QUP_TRACE=1 log only QUP SPI + BLSP1 BAM accesses, each with the
 *                        guest PC that touched the register.  Same lines as
 *                        QEMU_IPQ_TRACE=2 for those two blocks, without the
 *                        hundreds of megabytes the rest of the SoC adds.
 *   QEMU_IPQ_FLASH_SIZE  SPI NOR die size in bytes, power of two
 *                        1 MiB..64 MiB (default 32 MiB)
 *   QEMU_IPQ_FLASH_JEDEC RDID bytes as hex, e.g. "ef4019" or "20ba19"
 *                        (default: Winbond "ef40" + log2(size))
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
#include "qemu/units.h"
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
#include "system/reset.h"
#include "system/system.h"
#include "system/device_tree.h"
#include "system/runstate.h"
#include "hw/intc/arm_gic.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "cpu.h"
#include "net/net.h"
#include "net/checksum.h"
#include "net/eth.h"
#include <libfdt.h>

#define IPQ_ROM_BASE      0x00000000u
#define IPQ_ROM_SIZE      0x00040000u
#define IPQ_PRNG_BASE     0x00022000u
#define IPQ_GCNT_BASE     0x004a1000u
#define IPQ_PSHOLD_BASE   0x004ab000u
#define IPQ_WDT_BASE      0x0b017000u
#define IPQ_TLMM_BASE     0x01000000u
#define IPQ_TLMM_SIZE     0x00300000u
#define IPQ_GCC_BASE      0x01800000u
#define IPQ_GCC_SIZE      0x00060000u
/*
 * Consecutive reads of one freshly-written clock register before its bit 0 is
 * declared a completed command trigger rather than data (see IPQStub).  Must
 * stay well below the shortest poll budget any stage uses: the kernel's
 * update_config() in clk-rcg2.c gives up after 500 reads and warns.
 */
#define IPQ_SPIN_LIMIT    16
#define IPQ_UART0_BASE    0x078af000u
#define IPQ_UART1_BASE    0x078b0000u
#define IPQ_QUP_BASE      0x078b5000u
#define IPQ_BLSP1_BAM_BASE 0x07884000u
#define IPQ_BLSP1_BAM_SIZE 0x00023000u
#define IPQ_QPIC_BASE     0x07980000u
#define IPQ_QPIC_SIZE     0x00040000u
#define IPQ_RAM_BASE      0x80000000u
#define IPQ_QGIC_BASE     0x0b000000u
/* QGIC2 line count: 256 SPIs (the ipq40xx DTS uses up to 0xff) + 32 PPIs */
#define IPQ_GIC_NUM_IRQ   288u
/* Cortex-A7s in the DTS: cpu@0..cpu@3 (see the GIC note in ipq4019_init) */
#define IPQ_GIC_NUM_CPU   4u
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
    /* Bits that do not read back as written (write-only fields). */
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
     * Clock/reset blocks: bit 0 of the command registers is a "start the
     * update" trigger.  Software sets it and then polls until it reads 0.
     *
     * It cannot be modelled by masking bit 0 of every read-back: the data
     * registers of those same blocks keep bit 0 as real data (CFG_RCGR, whose
     * bit 0 is the LSB of the divider).  Nor can a write of exactly 1 be
     * treated as the trigger: SBL1's RCG helper does a read-modify-write and
     * issues 0xffffffff (bit 0 set) on registers it never wrote before.
     *
     * So the write lands as written, and a tight poll loop is taken as proof
     * that the bit is a trigger rather than data: after IPQ_SPIN_LIMIT
     * consecutive reads of the same freshly-written register with bit 0 still
     * set, the operation is declared complete and bit 0 reads back 0.
     */
    bool trig_bit0;
    uint32_t spin_idx;
    uint32_t spin;
    /*
     * GCC/APCS branch (CBCR) registers: bit 0 is the branch enable and bit 31
     * is its read-only CLK_OFF status, which is the complement of the enable.
     * The kernel's clk_branch2_check_halt() waits for CLK_OFF to *set* when it
     * disables a branch and warns "status stuck at 'on'" if it never does, so
     * an unwritten or masked bit 31 stalls every unused-clock disable.
     */
    bool cbc_status;
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

/*
 * Console input debugging.  Keystrokes only reach a shell prompt if the whole
 * chain works -- chardev -> model FIFO -> GIC -> msm_serial interrupt handler
 * -> tty flip buffer -> procd -- and when a byte disappears there is no trace
 * of it anywhere.  QEMU_IPQ_UART_RX_LOG=1 prints every step.
 */
#define UART_RX_LOG(...)                                                  \
    do {                                                                  \
        if (env_true("QEMU_IPQ_UART_RX_LOG")) {                           \
            fprintf(stderr, "uart-rx: " __VA_ARGS__);                     \
        }                                                                 \
    } while (0)

static const char *trace_pc(void)
{
    static char buf[48];
    CPUState *cs = current_cpu;

    if (!cs) {
        return "";
    }
    /*
     * LR as well as PC: the bootloaders do their MMIO through tiny helpers
     * (`ldr r0,[r0] / bx lr` at 0x81000050 in RouterBOOT), so the access PC
     * names the helper and only LR says which loop is asking.
     */
    snprintf(buf, sizeof(buf), " pc=%08x lr=%08x",
             (uint32_t)ARM_CPU(cs)->env.regs[15],
             (uint32_t)ARM_CPU(cs)->env.regs[14]);
    return buf;
}

/* Host milliseconds since QEMU started, for "when did that access happen". */
static const char *trace_ms(void)
{
    static char buf[24];

    snprintf(buf, sizeof(buf), " t=%" PRId64 "ms",
             g_get_monotonic_time() / 1000);
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
             * Written register.  read_mask models write-only bits: bit 31 of
             * the clock blocks is a busy flag that hardware clears when the
             * update finishes, so a write of 0x80000001 must not read back
             * busy-forever (SBL1's helper spins with `blt` until it clears).
             */
            v = s->shadow[idx] & s->read_mask;
            tag = "";
        } else {
            v = stub_def_read(s);
        }
        /*
         * Poll detector for the bit-0 command trigger: nothing else in the
         * block can make progress while software spins on one register, so a
         * long run of reads of the same just-written register means the bit is
         * a trigger and the update has completed.
         */
        if (s->trig_bit0 && idx == s->spin_idx && idx < s->nregs &&
            s->valid[idx] && !pinned) {
            if (++s->spin >= IPQ_SPIN_LIMIT && (s->shadow[idx] & 1)) {
                s->shadow[idx] &= ~1u;
                s->spin = 0;
                v = s->shadow[idx] & s->read_mask;
            }
        }
        /* branch registers report CLK_OFF as the complement of the enable */
        if (s->cbc_status && idx < s->nregs && s->valid[idx] && !pinned) {
            v = (v & ~0x80000000u) | ((v & 1) ? 0 : 0x80000000u);
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
    s->spin_idx = idx;
    s->spin = 0;
}

static const MemoryRegionOps stub_ops = {
    .read = stub_read,
    .write = stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* 8 so that LDRD/STRD pairs are split into two register accesses */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* Every stub register file, so a system reset can clear what software left */
static GPtrArray *ipq_stub_list;

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
    if (!ipq_stub_list) {
        ipq_stub_list = g_ptr_array_new();
    }
    g_ptr_array_add(ipq_stub_list, s);
    return s;
}

/*
 * PS_HOLD: how the guest restarts the board.
 *
 * The device tree hands Linux a `qcom,pshold` node at 0x4ab000.  The PMIC
 * keeps the rails up while PS_HOLD is high, so software asks for a restart by
 * dropping it -- the driver writes 0 and never comes back.  Model the register
 * (reads return the last value, reset value 1) and turn a write that clears
 * bit 0 into a machine reset, the same event the reset button causes.  Without
 * it the kernel prints "Reboot failed -- System halted", and there is no way
 * for the guest to ever check that its NOR writes survived a restart.
 */
typedef struct IPQPsHold {
    uint32_t val;
} IPQPsHold;

static IPQPsHold *ipq_pshold;

static uint64_t pshold_read(void *opaque, hwaddr off, unsigned size)
{
    IPQPsHold *s = opaque;

    return s->val;
}

static void pshold_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IPQPsHold *s = opaque;

    if (env_true("QEMU_IPQ_PSHOLD_LOG")) {
        qemu_log_mask(LOG_UNIMP, "pshold: write 0x%08x%s\n", (uint32_t)v,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    s->val = v;
    if (!(v & 1) && !env_true("QEMU_IPQ_NO_PSHOLD_RESET")) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static const MemoryRegionOps pshold_ops = {
    .read = pshold_read,
    .write = pshold_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * KPSS watchdog at 0x0b017000: how RouterBOOT restarts the board.
 *
 * Exiting the setup menu calls a routine that records a reason and arms this
 * block, then spins in `b .` waiting for the chip to go away:
 *
 *   0x81001acc  [base+0x08] = 0        ; clear status
 *               [base+0x04] = 1        ; enable
 *               [base+0x14] = 0x100    ; bite time, 32.768 kHz ticks
 *               [base+0x10] = -1       ; bark time, never
 *               [base+0x08] = 1        ; kick
 *   0x81001b28  bl 0x81001acc(0x100) / b .
 *
 * The dump's own device tree says what the block is -- `watchdog@b017000`,
 * compatible "qcom,kpss-wdt" -- and U-Boot pokes the same page right before
 * handing over to the kernel ([base] = 1, [base+0x24] = 0xea600) but never
 * writes the enable at +0x04, so arming on the enable is what tells the two
 * apart.  Modelling it as a plain stub left the bootloop hung forever after
 * leaving setup, because nothing ever performed the reset it was waiting for.
 */
#define WDT_RST        0x00
#define WDT_EN         0x04
#define WDT_STS        0x08
#define WDT_BARK_TIME  0x10
#define WDT_BITE_TIME  0x14

typedef struct IPQWdt {
    MemoryRegion mr;
    uint32_t regs[0x1000 / 4];
    uint32_t hz;                /* sleep clock, ticks per second */
    QEMUTimer *bite;
} IPQWdt;

static IPQWdt *ipq_wdt;

static uint64_t wdt_read(void *opaque, hwaddr off, unsigned size)
{
    IPQWdt *w = opaque;

    return w->regs[(off & 0xfff) / 4];
}

static void wdt_bite(void *opaque)
{
    IPQWdt *w = opaque;

    if (env_true("QEMU_IPQ_WDT_LOG")) {
        qemu_log_mask(LOG_UNIMP, "wdt: bite, requesting reset\n");
    }
    w->regs[WDT_STS / 4] = 1;
    if (!env_true("QEMU_IPQ_NO_WDT_RESET")) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static void wdt_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IPQWdt *w = opaque;
    unsigned idx = (off & 0xfff) / 4;

    if (env_true("QEMU_IPQ_WDT_LOG")) {
        qemu_log_mask(LOG_UNIMP, "wdt: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", off, v, trace_pc());
    }
    w->regs[idx] = v;

    /*
     * The counter runs from the enable, and any later write is a kick: the
     * bootloader writes the bite time after enabling, so arming on the enable
     * alone would use the reset value.
     */
    if ((off & 0xfff) == WDT_EN && !(v & 1)) {
        timer_del(w->bite);
        return;
    }
    if (w->regs[WDT_EN / 4] & 1) {
        uint32_t ticks = w->regs[WDT_BITE_TIME / 4];

        if (!ticks) {
            ticks = 1;
        }
        timer_mod(w->bite, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)ticks * NANOSECONDS_PER_SECOND / w->hz);
    }
}

static const MemoryRegionOps wdt_ops = {
    .read = wdt_read,
    .write = wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * Panic capture.
 *
 * RouterOS 7.17 keeps kernel messages in memory (logring.ko) instead of
 * printing them, so a crash on the board is invisible -- except that its
 * panic handler stores the text in DRAM ("save panic in DRAM").  The reset
 * that follows is the last moment that copy exists, because the next boot's
 * SBL1 reloads straight over it.  Dumping DRAM as the reset happens makes a
 * suppressed panic readable from the image itself, with nothing decoded or
 * patched:
 *
 *   QEMU_IPQ_RESET_DUMP=/tmp/ram.bin ./run.sh
 *   strings -n 8 /tmp/ram.bin | grep -n "Kernel panic"
 */
static MemoryRegion *ipq_dram_mr;
static uint64_t ipq_dram_size;

static void ipq_reset_dump(void *opaque)
{
    const char *path = getenv("QEMU_IPQ_RESET_DUMP");
    const void *ram;
    FILE *f;

    if (!path || !*path || !ipq_dram_mr) {
        return;
    }
    ram = memory_region_get_ram_ptr(ipq_dram_mr);
    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ipq4019: cannot write reset dump %s\n", path);
        return;
    }
    fwrite(ram, 1, ipq_dram_size, f);
    fclose(f);
    fprintf(stderr, "ipq4019: dram dumped to %s at reset (%" PRIu64 " bytes)\n",
            path, ipq_dram_size);
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
/*
 * Every counter here reads "virtual clock * rate", so it counts from the
 * moment QEMU started.  A machine reset re-bases them, because the boot chain
 * stamps its own log from one of these: without it, SBL1's first line after a
 * guest reboot reads "13688788 - SBL1, Start" -- the seconds the machine had
 * already been up -- instead of something near zero.
 */
static uint64_t ipq_counter_epoch_ns;

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

typedef struct IPQQupl {
    MemoryRegion mr;
    uint32_t shadow[0x1000 / 4];

    /* SPI slave state machine (driven by every byte clocked out) */
    int phase;              /* 0 opcode, 1 address, 2 dummy, 3 data */
    uint8_t opcode;
    uint32_t addr;
    int addr_left;
    int dummy_left;
    int cs;                 /* die selected: cs_force || cs_gpio */
    int cs_force;           /* QUP_IO_CONTROL FORCE_CS (QCA boot drivers) */
    int cs_gpio;            /* cs-gpios pin driven through the TLMM */
    int read_data;          /* stream data out of the flash */
    uint32_t rx_off;        /* next flash offset of the data stream */
    uint8_t rx_buf[8];      /* short replies: JEDEC id, status */
    int rx_len;
    int rx_pos;
    uint8_t status;         /* SR1: bit0 busy, bit1 WEL, 2-4 BP, 5 TB, 6 SEC */
    uint8_t status2;        /* SR2: bit5 E_FAIL, bit6 P_FAIL (W25Q style) */
    int write_sr;           /* status bytes still expected (0 = not WRSR) */
    uint32_t pgm_addr;      /* next die offset a PP data byte programs */
    uint32_t pgm_left;      /* bytes left before the page boundary */
    int wip_pending;        /* the die still owes one "busy" status read */
    int wr_kind;            /* NOR_WR_*: what the latched command will write */
    uint32_t data_bytes;    /* bytes shifted out of this command's reply */
    uint32_t last_addr;     /* decoded address of the previous command */
    /*
     * Die state, not controller state: once 0xB7 (Enter 4-Byte Address Mode)
     * has been accepted the flash keeps using four address bytes for the
     * standard opcodes until 0xE9, even across chip-select edges.  Linux's
     * spi-nor does exactly this on a >16 MiB die and then issues plain 0x03
     * with a 32-bit address, so decoding that as three bytes silently shifts
     * every read by one byte (0x00180000 became 0x00001800).
     */
    int addr4;

    /* MISO queue: one entry per byte clocked out, popped by FIFO reads */
    uint8_t *resp;
    int resp_head;
    int resp_len;
    uint32_t resp_cap;
    uint64_t clocked;

    /*
     * QUP transfer engine, as the Linux driver drives it: MX_*_CNT programs
     * how many SPI words this transfer carries, the state machine runs it,
     * and QUP_OPERATIONAL reports the FIFO/done flags the ISR polls.
     */
    uint32_t state;             /* QUP_STATE_RESET/RUN/PAUSE */
    uint32_t op_mask;           /* QUP_OPERATIONAL_MASK */
    uint32_t tx_bytes;          /* bytes written to the output FIFO */
    uint32_t rx_bytes;          /* bytes read from the input FIFO */
    uint32_t clocks;            /* bytes clocked out in this transfer */
    uint32_t max_in_done;       /* sticky MAX_INPUT_DONE */
    uint32_t max_out_done;      /* sticky MAX_OUTPUT_DONE */
    qemu_irq irq;               /* SPI 95, level high */
} IPQQupl;

static IPQMem *ipq_flash;
static uint32_t ipq_flash_die_size = IPQ_FLASH_DEFAULT;
static uint8_t ipq_flash_jedec[4] = { 0xef, 0x40, 0x19, 0x00 };
static int ipq_flash_jedec_len = 3;
static IPQQupl *qup_spi;         /* the one QUP the BAM pipes are wired to */

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
static bool die_dbg_on(void);

static void die_dbg_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void die_dbg_log(const char *fmt, ...)
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
static void die_dbg_ser(uint8_t c)
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

static bool die_dbg_on(void)
{
    const char *v = getenv("QEMU_IPQ_DIE_LOG");

    /* "0"/"" mean off, like every other knob here; "1" and paths mean on. */
    return v && *v && strcmp(v, "0") != 0;
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
    if (!ipq_flash || off >= ipq_flash->size) {
        return 0xff;
    }
    return ipq_flash->data[off];
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

static void nor_erase(IPQQupl *q, uint32_t off, uint32_t len)
{
    if (!ipq_flash) {
        return;
    }
    off &= ipq_flash_die_size - 1;
    if (off >= ipq_flash->size) {
        return;
    }
    len = MIN(len, (uint32_t)(ipq_flash->size - off));
    if (nor_range_protected(q, off, len)) {
        q->status2 |= NOR_SR2_EFAIL;
        return;
    }
    memset(ipq_flash->data + off, 0xff, len);
    if (env_true("QEMU_IPQ_SPI_LOG")) {
        qemu_log_mask(LOG_UNIMP, "die: erase 0x%06x +0x%x\n",
                      off, (unsigned)len);
    }
}

static void nor_program_byte(IPQQupl *q, uint8_t b)
{
    uint32_t page;

    if (!ipq_flash || !q->pgm_left || q->pgm_addr >= ipq_flash->size) {
        return;
    }
    if (nor_range_protected(q, q->pgm_addr, 1)) {
        q->status2 |= NOR_SR2_PFAIL;
        return;
    }
    /* Programming can only clear bits; a set bit needs an erase first. */
    ipq_flash->data[q->pgm_addr] &= b;
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
        /* Whatever die the board has: QEMU_IPQ_FLASH_JEDEC/_SIZE. */
        memcpy(q->rx_buf, ipq_flash_jedec, ipq_flash_jedec_len);
        q->rx_len = ipq_flash_jedec_len;
        break;
    case 0x4b:                          /* Read Unique ID: 8 bytes */
        q->rx_len = 8;
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
            if (q->wr_kind) {
                nor_write_start(q);
            }
            die_dbg_n = 0;
            if (die_dbg_on()) {
                die_dbg_log("die: xact op=0x%02x addr=0x%08x pc=%08x\n",
                            q->opcode, q->addr,
                            (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
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
        if (q->write_sr) {
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
    if (q->read_data) {
        uint8_t v = qup_flash_byte(q->rx_off++ & (ipq_flash_die_size - 1));

        q->data_bytes++;
        die_dbg_note(v);
        return v;
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

    if (die_dbg_on() && q->phase <= 2) {
        die_dbg_log("die: mosi %02x phase=%d pc=%08x\n", mosi, q->phase,
                    (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
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
static uint32_t qup_in_word(IPQQupl *q)
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
    q->rx_off = 0;
    q->rx_len = 0;
    q->rx_pos = 0;
    qup_fifo_flush(q);
}

/*
 * The die has exactly one chip select, driven from two places depending on
 * who is talking to the QUP: QCA's boot drivers poke FORCE_CS in
 * SPI_IO_CONTROL, while Linux uses the gpio descriptor from cs-gpios and
 * leaves SPI_IO_CONTROL alone.  Both are ORed into the same pin, and an edge
 * of the result is what starts or ends a die-level transaction.
 */
static void qup_cs_source(IPQQupl *q, int *src, int level)
{
    int cs;

    if (*src == level) {
        return;
    }
    *src = level;
    cs = q->cs_force || q->cs_gpio;
    if (cs != q->cs) {
        q->cs = cs;
        if (die_dbg_on()) {
            /*
             * "sel" is the assertion state of the NOR's chip select, not its
             * index: the die sits on CS0 and CS0 is active low, so sel=1
             * means CS0 is driven low and the die is listening.  The line
             * itself is printed by the pin54 tracer above.
             */
            die_dbg_log("die: cs0 sel=%d op=0x%02x addr=0x%08x n=%u"
                        " first=%02x %02x %02x %02x\n", cs, qup_spi->opcode,
                        qup_spi->last_addr, die_dbg_n,
                        die_dbg_first[0], die_dbg_first[1],
                        die_dbg_first[2], die_dbg_first[3]);
        }
        qup_bus_reset(q);
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
                    (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
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
        qup_cs_source(q, &q->cs_force,
                      (val & QUP_IO_CONTROL_FORCE_CS) != 0);
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

/* ------------------------------------------------------------------ */
/* TLMM GPIO/pinctrl                                                   */
/* ------------------------------------------------------------------ */

/*
 * The pin controller is a wall of 0x1000-byte per-pin register blocks:
 *
 *   0x000 + 0x1000 * pin   CTL      mux[3:2], pull[1:0], drive[8:6], OE[9]
 *   0x004 + 0x1000 * pin   IO       in[0], out[1], interrupt config above
 *   0x008 + 0x1000 * pin   INTR_CFG / INTR_TARGET
 *   0x00c + 0x1000 * pin   INTR_STATUS
 *
 * (pinctrl-ipq4019.c: .ctl_reg = 0x1000 * id, .io_reg = 0x4 + 0x1000 * id.)
 *
 * Only one bit in this block is load-bearing for the machine: the SPI NOR
 * chip select.  The OpenWrt DTS describes it as
 *
 *     cs-gpios = <&tlmm 54 GPIO_ACTIVE_LOW>;
 *
 * so the SPI core leaves host->set_cs alone and drives pin 54 through
 * gpiolib instead.  Modelling the IO register is therefore what gives the
 * flashing die its chip-select edges: without it every QUP state RESET would
 * have to be guessed as a transaction boundary, and one SPI message with a
 * separate command and data transfer (which is exactly how spi-mem issues a
 * read) would be chopped in half.
 *
 * The rest of the block only needs to read back what was written: gpiolib
 * does read-modify-writes on these registers.
 */
#define IPQ_TLMM_IO_OFFSET   0x004u
#define IPQ_TLMM_IO_OUT_BIT  1

typedef struct IPQTLMM {
    MemoryRegion mr;
    uint32_t *regs;
    uint32_t nregs;
} IPQTLMM;

static IPQTLMM *ipq_tlmm;
static unsigned ipq_tlmm_cs_pin = 54;

/* Chip select changed: an active-low CS edge starts/ends a die transaction. */
static void ipq_tlmm_cs_changed(void)
{
    IPQTLMM *t = ipq_tlmm;
    uint32_t v;
    int assert;

    if (!t || !qup_spi || t->nregs <= ipq_tlmm_cs_pin * 0x400 + 1) {
        return;
    }
    v = t->regs[(ipq_tlmm_cs_pin * 0x1000 + IPQ_TLMM_IO_OFFSET) >> 2];
    /* GPIO_ACTIVE_LOW: the pin low is the asserted chip select */
    assert = !((v >> IPQ_TLMM_IO_OUT_BIT) & 1);
    if (die_dbg_on()) {
        die_dbg_log("die: pin%u io=0x%08x cs-gpio=%d\n",
                    ipq_tlmm_cs_pin, v, assert);
    }
    qup_cs_source(qup_spi, &qup_spi->cs_gpio, assert);
}

static uint64_t tlmm_read(void *opaque, hwaddr off, unsigned size)
{
    IPQTLMM *t = opaque;
    uint32_t idx = off >> 2;
    uint32_t v = idx < t->nregs ? t->regs[idx] : 0xffffffffu;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "tlmm: read  off 0x%06" HWADDR_PRIx
                      " -> 0x%08x\n", off, v);
    }
    return v;
}

static void tlmm_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQTLMM *t = opaque;
    uint32_t idx = off >> 2;
    unsigned pin = off >> 12;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "tlmm: write off 0x%06" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "\n", off, val);
    }
    if (idx < t->nregs) {
        t->regs[idx] = val;
    }
    if ((off & 0xfff) == IPQ_TLMM_IO_OFFSET && pin == ipq_tlmm_cs_pin) {
        ipq_tlmm_cs_changed();
    }
}

static const MemoryRegionOps tlmm_ops = {
    .read = tlmm_read,
    .write = tlmm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void add_tlmm(MemoryRegion *parent, hwaddr base, hwaddr size,
                     const char *name)
{
    IPQTLMM *t = g_new0(IPQTLMM, 1);
    uint32_t i;

    t->nregs = size >> 2;
    t->regs = g_new0(uint32_t, t->nregs);
    /*
     * Unimplemented pins answer ~0 exactly like the decode-only stub this
     * replaced: the kernel only ever touches the pins its own pinctrl state
     * describes, and the GPIO_IO reset value of all ones keeps every chip
     * select deasserted until gpiolib drives it.
     */
    for (i = 0; i < t->nregs; i++) {
        t->regs[i] = 0xffffffffu;
    }
    memory_region_init_io(&t->mr, NULL, &tlmm_ops, t, name, size);
    memory_region_add_subregion(parent, base, &t->mr);
    ipq_tlmm = t;
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

/*
 * { u32 addr; u16 size; u16 flags } -- the layout QCA's bam.h documents and the
 * one Linux's struct bam_desc_hw uses.  The length is the 16-bit size field;
 * bits 16.. are flags (EOT/INT/NWD/CMD) and must not end up in the count.
 */
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

/*
 * Which peripheral a BAM pipe is wired to.  The SoC fixes this in silicon (the
 * BLSP routes each QUP to a pipe pair) and the board DTB spells it out with
 * "dmas"/"dma-names": on the IPQ40xx the console UART is pipes 0 (tx) / 1 (rx)
 * and the SPI the boot flash hangs off is pipes 4 (tx) / 5 (rx).
 *
 * This matters because the flash die must only ever be driven by the pipe that
 * really carries SPI.  The kernel's console driver keeps a serial RX transfer
 * queued on pipe 1 at all times -- completing it against the SPI die invents
 * SPI clocks on the wire (opcode 0x00, address 0x000000, ...) and leaves the
 * die in the middle of a bogus transaction, which is exactly how a perfectly
 * good rootfs read can come back as zeros.
 */
typedef enum {
    BAM_CLIENT_NONE,            /* no data source: leave the ring alone */
    BAM_CLIENT_SPI,             /* talk to the QUP SPI (the NOR die) */
    BAM_CLIENT_UART_TX,         /* push bytes to the serial backend */
    BAM_CLIENT_UART_RX,         /* wait for console input */
} BAMClient;

typedef struct IPQBamPipe {
    BAMClient client;
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
    /*
     * The receive descriptor the engine is filling right now.  Real hardware
     * keeps writing into the posted buffer as bytes arrive and only retires
     * it when the buffer fills or the UART goes idle (the stale timeout), so
     * one burst of console input completes one descriptor -- not one per
     * chardev read.
     */
    uint32_t rx_desc_addr;
    uint16_t rx_desc_len;
    uint16_t rx_desc_got;
    bool rx_desc_open;
} IPQBamPipe;

typedef struct IPQBam {
    MemoryRegion mr;
    uint32_t *shadow;           /* everything not explicitly modelled */
    uint8_t *valid;
    uint32_t nregs;
    uint32_t irq_srcs_msk;      /* BAM_IRQ_SRCS_MSK of EE0 */
    qemu_irq irq;               /* SPI 238, level high */
    IPQBamPipe pipe[BAM_PIPE_COUNT];
} IPQBam;

/*
 * Pipe wiring: which peripheral each BAM pipe belongs to.  Defaults are the
 * IPQ40xx BLSP layout used by every ipq40xx board's boot flash and console
 * (console UART = 0/1, boot SPI = 4/5); QEMU_IPQ_BAM_SPI_PIPES and
 * QEMU_IPQ_BAM_UART_PIPES override them for a board that wires them
 * differently.
 */
static void bam_set_pipe_list(IPQBam *b, const char *env, BAMClient client)
{
    const char *spec = getenv(env);
    const char *p;

    if (!spec || !*spec) {
        return;
    }
    for (p = spec; *p; ) {
        long n;
        char *end;

        n = strtol(p, &end, 0);
        if (end == p) {
            break;
        }
        if (n >= 0 && n < BAM_PIPE_COUNT) {
            b->pipe[n].client = client;
        }
        p = (*end == ',') ? end + 1 : end;
    }
}

static void bam_wire_pipes(IPQBam *b)
{
    static const int spi_default[] = { 4, 5 };
    static const int uart_default[] = { 0, 1 };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(spi_default); i++) {
        b->pipe[spi_default[i]].client = BAM_CLIENT_SPI;
    }
    /* Console UART: tx goes out to the backend, rx waits for input. */
    b->pipe[uart_default[0]].client = BAM_CLIENT_UART_TX;
    b->pipe[uart_default[1]].client = BAM_CLIENT_UART_RX;

    if (getenv("QEMU_IPQ_BAM_SPI_PIPES") ||
        getenv("QEMU_IPQ_BAM_UART_PIPES")) {
        for (i = 0; i < BAM_PIPE_COUNT; i++) {
            b->pipe[i].client = BAM_CLIENT_NONE;
        }
        if (!getenv("QEMU_IPQ_BAM_SPI_PIPES")) {
            for (i = 0; i < ARRAY_SIZE(spi_default); i++) {
                b->pipe[spi_default[i]].client = BAM_CLIENT_SPI;
            }
        }
        if (!getenv("QEMU_IPQ_BAM_UART_PIPES")) {
            b->pipe[uart_default[0]].client = BAM_CLIENT_UART_TX;
            b->pipe[uart_default[1]].client = BAM_CLIENT_UART_RX;
        }
    }
    bam_set_pipe_list(b, "QEMU_IPQ_BAM_SPI_PIPES", BAM_CLIENT_SPI);
    bam_set_pipe_list(b, "QEMU_IPQ_BAM_UART_PIPES", BAM_CLIENT_UART_TX);
}


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

static void bam_update_irq(IPQBam *b);
static bool bam_uart_rx_move(IPQBam *b, int n);

static bool bam_pipe_lookup(hwaddr off, int *n, hwaddr *roff)
{
    if (off < BAM_PIPE_FIRST || off >= IPQ_BLSP1_BAM_SIZE) {
        return false;
    }
    *n = (off - BAM_PIPE_FIRST) / BAM_PIPE_STRIDE;
    *roff = (off - BAM_PIPE_FIRST) % BAM_PIPE_STRIDE;
    return *n < BAM_PIPE_COUNT;
}

/* Defined with the UART model; lets the BAM TX pipe drive the same backend. */
static void uart_bam_write(const uint8_t *buf, size_t n);

/*
 * The console receive path, also defined with the UART model.  Linux's
 * msm_serial does not use the RX FIFO interrupt path on this SoC: it requests
 * a DMA channel, masks RXLEV/RXSTALE, sets RX_BAM_ENABLE in UARTDM_DMEN and
 * then only reads a batch of bytes out of its DMA buffer when the engine says
 * a transfer finished.  So input reaches userspace only if the BAM RX pipe is
 * modelled.
 */
static bool uart_bam_rx_armed(void);
static size_t uart_bam_rx_pending(void);
static size_t uart_bam_rx_pull(uint8_t *buf, size_t n);
static void uart_bam_rx_account(size_t n);
static void bam_uart_rx_kick(void);
static void bam_uart_rx_idle(void);
static void bam_uart_rx_input_ready(void);
static void uart_rx_idle_arm(void);

/* The BLSP1 BAM, so the UART model can kick the RX pipe when input lands. */
static IPQBam *ipq_blsp1_bam;

static const char *bam_client_name(BAMClient c, uint32_t ctrl)
{
    switch (c) {
    case BAM_CLIENT_SPI:
        return "spi";
    case BAM_CLIENT_UART_TX:
        return "uart-tx";
    case BAM_CLIENT_UART_RX:
        return "uart-rx";
    default:
        return "idle";
    }
}

static void bam_pipe_xfer(IPQBam *b, int n, uint32_t addr, uint32_t len)
{
    IPQBamPipe *p = &b->pipe[n];
    uint32_t done, i;
    uint8_t buf[256];

    if (p->client == BAM_CLIENT_UART_TX) {
        /*
         * Console output: hand the queued bytes to the serial backend.  The
         * driver maps them for DMA, so they only reach the wire through this
         * pipe.
         */
        for (done = 0; done < len; done += i) {
            i = MIN(len - done, (uint32_t)sizeof(buf));
            address_space_read(&address_space_memory, addr + done,
                               MEMTXATTRS_UNSPECIFIED, buf, i);
            uart_bam_write(buf, i);
        }
        p->bytes += len;
        return;
    }

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
    /*
     * A pipe with no client has no data to move: on real hardware its
     * descriptors simply stay in the FIFO until the peripheral has something.
     * Left queued (and uncompleted) here too, so software sees exactly what it
     * would see on the board -- and, more importantly, nothing else in the
     * machine believes a transfer happened.
     */
    if (p->client == BAM_CLIENT_NONE) {
        return;
    }
    if (p->client == BAM_CLIENT_UART_RX) {
        /* Move whatever console input is waiting into the posted buffer. */
        bam_uart_rx_move(b, n);
        /* Input still sitting in the chardev queue (a burst longer than the
         * receive FIFO) can now flow in as well. */
        bam_uart_rx_input_ready();
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
        if (die_dbg_on()) {
            uint8_t head[8] = { 0 };
            int hn = MIN(len, (uint32_t)sizeof(head));
            int k;

            if (!(p->ctrl & BAM_CTRL_DIR_BAM2SYS)) {
                address_space_read(&address_space_memory, addr,
                                   MEMTXATTRS_UNSPECIFIED, head, hn);
            }
            die_dbg_log("die: bam desc pipe=%d %s addr=0x%08x len=%u"
                        " pc=%08x head=", n,
                        (p->ctrl & BAM_CTRL_DIR_BAM2SYS) ? "qup->ram" :
                        "ram->qup", addr, len,
                        (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
            for (k = 0; k < 8; k++) {
                die_dbg_log("%02x", head[k]);
            }
            die_dbg_log("\n");
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
    bam_update_irq(b);
}

/*
 * Move pending console input into the RX pipe's posted descriptors.
 *
 * The engine fills the descriptor it has posted and retires it when the buffer
 * is full or when the UART reports that the line has gone idle (the stale
 * timeout) -- the second case is how a driver learns about a short burst.  A
 * pasted line therefore completes exactly one descriptor, which matters
 * because msm_serial advances its tty data by `RX_TOTAL_SNAP - old_snap`;
 * retiring per chardev read would split one line over two buffers and make
 * that delta point at the wrong bytes.
 */
static void bam_uart_rx_retire(IPQBam *b, IPQBamPipe *p)
{
    uint32_t d[2];

    if (!p->rx_desc_open) {
        return;
    }
    address_space_read(&address_space_memory, p->ring_base + p->cons,
                       MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
    /* The engine writes the real length back into the descriptor. */
    d[1] = (d[1] & 0xffff0000u) | p->rx_desc_got;
    address_space_write(&address_space_memory, p->ring_base + p->cons,
                        MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
    p->rx_desc_open = false;
    p->rx_desc_got = 0;
    p->cons += BAM_DESC_SIZE;
    if (p->cons >= p->ring_size) {
        p->cons = 0;
    }
    p->event |= env_u32("QEMU_IPQ_BAM_EVENT", BAM_EV_COMPLETE);
    p->irq_stts |= BAM_P_IRQ_PRCSD_DESC | BAM_P_IRQ_TRNSFR_END;
    bam_update_irq(b);
}

static bool bam_uart_rx_move(IPQBam *b, int n)
{
    IPQBamPipe *p = &b->pipe[n];
    bool moved = false;

    UART_RX_LOG("bam rx move pipe=%d ctrl=%x base=%08x size=%x cons=%x "
                "prod=%x armed=%d pending=%u open=%d got=%u\n", n, p->ctrl,
                p->ring_base, p->ring_size, p->cons, p->prod,
                uart_bam_rx_armed(), (unsigned)uart_bam_rx_pending(),
                p->rx_desc_open, p->rx_desc_got);
    if (!(p->ctrl & BAM_CTRL_ENABLE) || !(p->ctrl & BAM_CTRL_DIR_BAM2SYS)) {
        return false;
    }
    if (!p->ring_base || !p->ring_size || !uart_bam_rx_armed()) {
        return false;
    }

    for (;;) {
        uint32_t d[2];
        uint8_t buf[256];
        size_t room, take, got;

        if (!p->rx_desc_open) {
            if (p->cons == p->prod) {
                break;              /* the driver has not posted a buffer */
            }
            if (p->cons + BAM_DESC_SIZE > p->ring_size) {
                p->cons = 0;
                continue;
            }
            address_space_read(&address_space_memory, p->ring_base + p->cons,
                               MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
            if (!(d[1] & 0xffffu)) {
                break;
            }
            p->rx_desc_open = true;
            p->rx_desc_addr = d[0];
            p->rx_desc_len = d[1] & 0xffffu;
            p->rx_desc_got = 0;
        }
        if (!uart_bam_rx_pending()) {
            break;                  /* nothing arrived yet: leave it queued */
        }
        room = p->rx_desc_len - p->rx_desc_got;
        take = MIN(room, sizeof(buf));
        got = uart_bam_rx_pull(buf, take);
        if (!got) {
            break;
        }
        address_space_write(&address_space_memory,
                            p->rx_desc_addr + p->rx_desc_got,
                            MEMTXATTRS_UNSPECIFIED, buf, got);
        uart_bam_rx_account(got);
        p->rx_desc_got += got;
        moved = true;
        UART_RX_LOG("bam rx: %zu byte(s) '%c' -> guest %08x (got %u/%u)\n",
                    got, buf[0], p->rx_desc_addr, p->rx_desc_got,
                    p->rx_desc_len);
        if (p->rx_desc_got >= p->rx_desc_len) {
            bam_uart_rx_retire(b, p);   /* buffer full: hand it back now */
            continue;
        }
        /*
         * Keep it open until the line goes idle, and make sure that idle
         * timeout is actually running: the bytes may have arrived before the
         * driver posted this buffer, in which case the timer has already
         * fired once and would never retire the descriptor.
         */
        uart_rx_idle_arm();
        break;
    }
    if (moved && env_true("QEMU_IPQ_BAM_LOG")) {
        fprintf(stderr, "bam: uart-rx pipe=%d cons=%u prod=%u\n",
                n, p->cons, p->prod);
    }
    return moved;
}

/* The line went idle: flush the FIFO and retire the open descriptor. */
static void bam_uart_rx_idle(void)
{
    IPQBam *b = ipq_blsp1_bam;
    int i;

    if (!b) {
        return;
    }
    for (i = 0; i < BAM_PIPE_COUNT; i++) {
        if (b->pipe[i].client != BAM_CLIENT_UART_RX) {
            continue;
        }
        bam_uart_rx_move(b, i);
        if (b->pipe[i].rx_desc_open && b->pipe[i].rx_desc_got) {
            bam_uart_rx_retire(b, &b->pipe[i]);
        }
    }
}

/* Called from the UART when console input has been sitting idle. */
static void bam_uart_rx_kick(void)
{
    IPQBam *b = ipq_blsp1_bam;
    int i;

    if (!b) {
        return;
    }
    for (i = 0; i < BAM_PIPE_COUNT; i++) {
        if (b->pipe[i].client == BAM_CLIENT_UART_RX) {
            bam_uart_rx_move(b, i);
        }
    }
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
 * The global line is the AND of the per-pipe sources with the per-EE mask
 * (BAM_IRQ_SRCS_MSK), level-high.  Linux's bam_dma expects it to stay
 * asserted until it has cleared every pipe's BAM_P_IRQ_STTS.
 */
static void bam_update_irq(IPQBam *b)
{
    qemu_set_irq(b->irq, (bam_irq_srcs(b) & b->irq_srcs_msk) != 0);
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

/*
 * The two identification registers are properties of the silicon, not state
 * software leaves behind, so they are seeded whenever the block comes out of
 * reset -- both at machine init and on every later system reset.  Without the
 * seed the reads fall back to bam_def_read(), and SBL1's flash driver rejects
 * the revision's step field (error 303d at boot_flash_init).
 */
static void bam_seed_id(IPQBam *b)
{
    b->shadow[0x1000 >> 2] = (env_u32("QEMU_IPQ_BAM_EES", 4) << 8) |
                             env_u32("QEMU_IPQ_BAM_VER", 7);
    b->valid[0x1000 >> 2] = 1;
    b->shadow[0x1008 >> 2] = env_u32("QEMU_IPQ_BAM_PIPES", 12);
    b->valid[0x1008 >> 2] = 1;
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
        case BAM_P_EVNT_REG:        v = p->prod; break;
        case BAM_P_SW_OFSTS:
            v = p->cons;
            if (die_dbg_on()) {
                die_dbg_log("die: bam sw_ofsts read pipe=%d -> 0x%x\n", n, v);
            }
            break;
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
    if (env_true("QEMU_IPQ_TRACE") || env_true("QEMU_IPQ_QUP_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "blsp1-bam: read  off 0x%03" HWADDR_PRIx
                      " -> 0x%08x%s\n", off, v,
                      (env_u32("QEMU_IPQ_TRACE", 0) > 1 ||
                       env_true("QEMU_IPQ_QUP_TRACE")) ? trace_pc() : "");
    }
    return v;
}

static void bam_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQBam *b = opaque;
    uint32_t idx = off >> 2;
    hwaddr roff;
    int n;

    if (env_true("QEMU_IPQ_TRACE") || env_true("QEMU_IPQ_QUP_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "blsp1-bam: write off 0x%03" HWADDR_PRIx
                      " <- 0x%08" PRIx64 "%s\n", off, val,
                      (env_u32("QEMU_IPQ_TRACE", 0) > 1 ||
                       env_true("QEMU_IPQ_QUP_TRACE")) ? trace_pc() : "");
    }
    if (env_true("QEMU_IPQ_BAM_LOG") && off >= BAM_PIPE_FIRST &&
        off < IPQ_BLSP1_BAM_SIZE) {
        int ln = (off - BAM_PIPE_FIRST) / BAM_PIPE_STRIDE;
        hwaddr loff = (off - BAM_PIPE_FIRST) % BAM_PIPE_STRIDE;

        fprintf(stderr, "bam-w: pipe=%d off=%03x val=%08x cl=%s%s\n",
                ln, (unsigned)loff, (uint32_t)val,
                bam_client_name(b->pipe[ln].client, b->pipe[ln].ctrl),
                trace_pc());
    }
    if (idx < b->nregs) {
        b->shadow[idx] = val;
        b->valid[idx] = 1;
    }
    if (off == BAM_IRQ_SRCS_MSK) {
        b->irq_srcs_msk = val;
        bam_update_irq(b);
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
            bam_update_irq(b);
        }
        break;
    case BAM_P_IRQ_CLR:
        b->pipe[n].irq_stts &= ~(uint32_t)val;   /* write-one-to-clear */
        bam_update_irq(b);
        break;
    case BAM_P_IRQ_EN:
        b->pipe[n].irq_en = val;
        bam_update_irq(b);
        break;
    case BAM_P_EVENT:
        b->pipe[n].event &= val;                /* write-one-to-clear */
        break;
    case BAM_P_DESC_FIFO_ADDR:
        b->pipe[n].ring_base = val;
        if (die_dbg_on()) {
            die_dbg_log("die: bam ring pipe=%d base=0x%08x pc=%08x\n",
                        n, (unsigned)val,
                        (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
        }
        break;
    case BAM_P_FIFO_SIZES:
        b->pipe[n].ring_size = val & 0xffffu;
        if (die_dbg_on()) {
            die_dbg_log("die: bam sizes pipe=%d val=0x%08x pc=%08x\n",
                        n, (unsigned)val,
                        (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
        }
        break;
    case BAM_P_EVNT_REG: {
        IPQBamPipe *p = &b->pipe[n];

        p->prod = val & 0xffffu;
        if (die_dbg_on()) {
            die_dbg_log("die: bam evnt pipe=%d val=0x%x cons=0x%x"
                        " prod=0x%x base=0x%08x size=0x%x dir=%s ctrl=0x%x"
                        " pc=%08x\n", n, (unsigned)val, p->cons, p->prod,
                        p->ring_base, p->ring_size,
                        bam_client_name(p->client, p->ctrl), p->ctrl,
                        (uint32_t)ARM_CPU(current_cpu)->env.regs[15]);
        }
        bam_pipe_run(b, n);
        break;
    }
    case BAM_P_SW_OFSTS:
        if (die_dbg_on()) {
            die_dbg_log("die: bam sw_ofsts write pipe=%d val=0x%x\n", n,
                        (unsigned)val);
        }
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
    return r;
}

/* ------------------------------------------------------------------ */
/* Ethernet DMA engine (IPQESS)                                        */
/* ------------------------------------------------------------------ */

/* Implemented with the MDIO/PHY model below; the packet path needs them first. */
static const char *ipq_port_name(int port);
static void ipq_eth_set_host_link(int switch_port, bool up);

/*
 * The EDMA is the DMA engine between the CPU and the switch that is built
 * into the IPQ4019: 16 TX and 16 RX descriptor rings, per-queue interrupt
 * status/mask registers, and one GIC SPI per queue (TX queues are SPIs
 * 0x41.. and RX queues 0xf0.. in the board DTS).
 *
 * The TX side has to be modelled for the machine to make progress at all.
 * Transmitting is a handshake: the driver writes a chain of 16-byte TPDs,
 * publishes the producer index in TPD_IDX, and then waits for the engine to
 * advance the consumer index and raise the queue interrupt.  With a plain
 * zeroed register file the engine never answered, so the queue filled up,
 * the stack called netif_tx_stop_queue(), and the kernel printed
 *
 *   ipqess-edma c080000.ethernet eth0: NETDEV WATCHDOG: CPU: 0:
 *   transmit queue 0 timed out 6000 ms
 *   ipqess-edma c080000.ethernet eth0: TX timeout on queue 0
 *
 * and then sat in the reset path, which is what held up procd and the login
 * prompt.  Here the engine consumes whatever the driver publishes, updates
 * the queue statistics and raises the queue's interrupt, which is all the
 * driver needs to reclaim the descriptors.
 *
 * No frame leaves the machine by default: without a -netdev the internal port
 * has no link partner, so descriptors are accounted for and dropped -- like a
 * MAC whose switch port is down.  Nothing is received either, so the RX rings
 * stay exactly as the driver programmed them.
 *
 * The packet path to the host lives in edma_tx_frame()/edma_rx_deliver()
 * below: the engine is attached to a QEMU -netdev (QEMU_IPQ_NETDEV, "net0" by
 * default) and one switch port is bridged to it (QEMU_IPQ_ETH_BRIDGE_PORT,
 * lan1 = port 5 by default).  The out-of-band DSA tag in word3 selects which
 * transmitted frames go out, and the port id in rrd1 tells the guest which
 * front-panel port a received frame arrived on.
 */
#define IPQ_EDMA_QUEUES          16
#define IPQ_EDMA_TPD_SIZE        16

#define EDMA_REG_RX_ISR          0x0200
#define EDMA_REG_TX_ISR          0x0208
#define EDMA_REG_RX_SW_CONS_Q(x) (0x0220 + ((x) << 2))
#define EDMA_REG_TX_SW_CONS_Q(x) (0x0240 + ((x) << 2))
#define EDMA_REG_RX_INT_MASK_Q(x) (0x0300 + ((x) << 2))
#define EDMA_REG_TX_INT_MASK_Q(x) (0x0340 + ((x) << 2))
#define EDMA_REG_TPD_RING_SIZE   0x041c
#define EDMA_REG_TPD_BASE_Q(x)   (0x0420 + ((x) << 2))
#define EDMA_REG_TPD_IDX_Q(x)    (0x0460 + ((x) << 2))
#define EDMA_REG_TX_STAT_PKT_Q(x) (0x0700 + ((x) << 3))
#define EDMA_REG_TX_STAT_BYTE_Q(x) (0x0704 + ((x) << 3))

#define EDMA_REG_RFD_BASE_Q(x)   (0x0950 + ((x) << 2))
#define EDMA_REG_RFD_IDX_Q(x)    (0x09b0 + ((x) << 2))
#define EDMA_REG_RX_DESC0        0x0a10

#define EDMA_TPD_PROD_MASK       0x0000ffffu
#define EDMA_TPD_CONS_SHIFT      16
#define EDMA_TPD_EOP             (1u << 31)

/*
 * word1 flags, straight out of the OpenWrt driver (patches 700/702 of
 * target/linux/ipq40xx).  The checksum block is the interesting part:
 *
 *   bits 1:0..17   checksum start, in 16-bit words from the frame start
 *   bits 31:18     checksum insert offset, in 16-bit words from frame start
 *
 * and when neither LSO bit is set the hardware is expected to sum the frame
 * from the start offset to the end of the L4 payload and write the result at
 * the insert offset.  Linux already left the pseudo-header sum in the TCP/UDP
 * checksum field when it marked the skb CHECKSUM_PARTIAL, so summing the
 * segment as-is (field included) is all the engine has to do.
 */
#define EDMA_TPD_PPPOE           0x00000100
#define EDMA_TPD_IP_CSUM         0x00000200
#define EDMA_TPD_TCP_CSUM        0x00000400
#define EDMA_TPD_UDP_CSUM        0x00000800
#define EDMA_TPD_CUSTOM_CSUM     0x00000c00
#define EDMA_TPD_LSO             0x00001000
#define EDMA_TPD_LSO_V2          0x00002000
#define EDMA_TPD_VLAN_TAGGED     0x00004000
#define EDMA_TPD_IPV4            0x00010000
/*
 * Both fields are only 13 bits wide: the driver shifts `cso >> 1` (a byte
 * offset, so a 13-bit word offset covers 16 KiB) and stores the checksum
 * offset the same way.  Bit 31 of word1 is EOP, so an unmasked shift turns it
 * into bit 13 of the checksum-offset field and the engine would try to write
 * the checksum far past the end of the frame.
 */
#define EDMA_TPD_CSUM_START_MASK 0x00001fff
#define EDMA_TPD_CSUM_OFF_SHIFT  18
#define EDMA_TPD_CSUM_OFF_MASK   0x00001fff

#define EDMA_TPD_CVLAN           16
#define EDMA_TPD_INS_CVLAN       17
#define EDMA_TPD_SVLAN           14
#define EDMA_TPD_INS_SVLAN       15
#define EDMA_TPD_PORT_SHIFT      18
#define EDMA_TPD_PORT_MASK       0x01fc0000
#define EDMA_TPD_FROM_CPU        (1u << 25)

#define EDMA_RX_RING_SIZE        128
#define EDMA_RX_HEAD_BUFF_SIZE   1540    /* IPQESS_RX_HEAD_BUFF_SIZE */
#define EDMA_RRD_SIZE            16
#define EDMA_TPD_SIZE            16
#define EDMA_RFD_PROD_MASK       0x00000fffu
#define EDMA_RFD_CONS_SHIFT      16
#define EDMA_RRD_VALID           0x8000
#define EDMA_RRD_CSUM_FAIL       0xc000
#define EDMA_MAX_FRAME           65536

/* Host round-trip tracing at the netdev boundary, off unless asked for. */
#define NET_TRACE(...)                                                     \
    do {                                                                   \
        if (env_true("QEMU_IPQ_NET_TRACE")) {                              \
            fprintf(stderr, "net: t=%" PRId64 " ",                         \
                    qemu_clock_get_ms(QEMU_CLOCK_REALTIME));               \
            fprintf(stderr, __VA_ARGS__);                                  \
        }                                                                  \
    } while (0)

#define EDMA_DBG(...)                                                     \
    do {                                                                  \
        if (env_true("QEMU_IPQ_EDMA_LOG")) {                              \
            fprintf(stderr, "edma: " __VA_ARGS__);                        \
        }                                                                 \
    } while (0)

typedef struct IPQEdma {
    MemoryRegion mr;
    uint32_t *regs;                 /* plain register file for everything else */
    uint32_t nregs;
    uint32_t tx_pkt[IPQ_EDMA_QUEUES];
    uint32_t tx_byte[IPQ_EDMA_QUEUES];
    qemu_irq irq_tx[IPQ_EDMA_QUEUES];
    qemu_irq irq_rx[IPQ_EDMA_QUEUES];
    int tx_level[IPQ_EDMA_QUEUES];

    /* Host backend and the switch port it is wired to. */
    NetClientState *nc;
    NICConf nic;
    MemReentrancyGuard rx_guard;
    int bridge_port;                /* switch port index, 1..5 */
    int host_link;
    uint8_t *txbuf;                 /* one assembled TX frame */
    uint8_t *segbuf;                /* one segment of a TSO super-packet */
    uint64_t tx_host;               /* frames handed to the netdev */
    uint64_t tx_drop_noport;        /* frame's port bitmap lacks bridge_port */
    uint64_t tx_drop_nolink;        /* netdev client down / detached */
    uint64_t rx_host;               /* frames taken from the netdev */
    uint64_t rx_dropped;
} IPQEdma;

static IPQEdma *ipq_edma;

static void edma_update_irqs(IPQEdma *e)
{
    unsigned q;

    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        bool tx = (e->regs[EDMA_REG_TX_ISR / 4] & (1u << q)) &&
                  (e->regs[EDMA_REG_TX_INT_MASK_Q(q) / 4] & 1u);
        bool rx = (e->regs[EDMA_REG_RX_ISR / 4] & (1u << q)) &&
                  (e->regs[EDMA_REG_RX_INT_MASK_Q(q) / 4] & 1u);

        if (e->irq_tx[q] && tx != e->tx_level[q]) {
            e->tx_level[q] = tx;
            EDMA_DBG("tx irq q=%u %d (isr=%08x mask=%08x)\n", q, tx,
                     e->regs[EDMA_REG_TX_ISR / 4],
                     e->regs[EDMA_REG_TX_INT_MASK_Q(q) / 4]);
            qemu_set_irq(e->irq_tx[q], tx);
        }
        if (e->irq_rx[q]) {
            qemu_set_irq(e->irq_rx[q], rx);
        }
    }
}

/* Hexdump into stderr, for the descriptor/frame reconnaissance knob. */
static void edma_hexdump(const char *tag, const void *buf, unsigned n)
{
    const uint8_t *p = buf;
    unsigned i;

    fprintf(stderr, "edma: %s", tag);
    for (i = 0; i < n; i++) {
        fprintf(stderr, "%s%02x", (i % 16) ? " " : "\n   ", p[i]);
    }
    fprintf(stderr, "\n");
}

/*
 * ------------------------------------------------------------------
 * The packet path
 * ------------------------------------------------------------------
 *
 * The 16 TPDs of the descriptor layout the driver writes are
 *
 *     struct ipqess_tx_desc {          offset
 *         __le16 len;                    0    payload length of this buffer
 *         __le16 svlan_tag;              2
 *         __le32 word1;                  4    flags (see EDMA_TPD_*), EOP = bit 31
 *         __le32 addr;                   8    guest physical address of the data
 *         __le32 word3;                 12    DSA port bitmap, VLAN insert
 *     } __packed __aligned(16);
 *
 * A frame is the chain of descriptors up to and including the one with EOP
 * set, so the engine has to assemble them before it can do anything with the
 * frame.  word3 carries the out-of-band DSA tag: bit (port + 18) selects the
 * switch port the frame leaves through and bit 25 says the tag came from the
 * CPU.  Only the framed port is wired to the host, so only frames tagged for
 * it are sent; the rest are accounted for and dropped, which is exactly what
 * happens to a frame aimed at a port with nothing plugged in.
 */

/* Big-endian (network order) ones-complement sum, starting from `sum`. */
static uint32_t edma_csum_add(uint32_t sum, const uint8_t *p, unsigned n)
{
    unsigned i;

    for (i = 0; i + 1 < n; i += 2) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (i < n) {
        sum += (uint32_t)p[i] << 8;
    }
    return sum;
}

static uint16_t edma_csum_fold(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)sum;
}

/* Length of the Ethernet header, counting any VLAN tags. */
static unsigned edma_l2_len(const uint8_t *f, size_t len)
{
    unsigned l2 = 14;

    while (len >= l2 + 2) {
        uint16_t ethertype = ((uint16_t)f[l2 - 2] << 8) | f[l2 - 1];

        if (ethertype != 0x8100 && ethertype != 0x88a8 &&
            ethertype != 0x9100) {
            break;
        }
        if (len < l2 + 4) {
            break;
        }
        l2 += 4;
    }
    return MIN(l2, len);
}

/*
 * Complete the L4 checksum.
 *
 * The descriptor carries the offset of the checksum field in word1 bits 31:18
 * (in 16-bit words) and the L4 header offset in bits 12:0, but those low bits
 * are shared with the flag bits (0x100..0x2000), so the start offset cannot be
 * recovered from word1 alone -- for a TCP segment over IPv4 the driver ORs
 * 0xc00 for "custom checksum" on top of 17 for "34 bytes".  Take the layout
 * from the packet instead: the L4 header starts right after the IP header,
 * which is also exactly what the driver computes for the frame offsets.
 *
 * The checksum field must be zeroed before summing: Linux fills it with the
 * complement of the pseudo-header sum for CHECKSUM_PARTIAL skbs, so summing
 * the segment as it arrives would count the pseudo-header twice.
 */
static void edma_tx_csum(uint8_t *f, size_t len, uint32_t word1)
{
    size_t off = ((word1 >> EDMA_TPD_CSUM_OFF_SHIFT) &
                  EDMA_TPD_CSUM_OFF_MASK) * 2;
    unsigned l2 = edma_l2_len(f, len);
    const uint8_t *ip = f + l2;
    size_t cso, l4len;
    uint32_t sum;
    uint16_t csum;

    if (len < l2 + 20) {
        return;
    }
    if ((ip[0] >> 4) == 4) {
        unsigned ihl = (ip[0] & 0x0f) * 4;
        unsigned total = ((unsigned)ip[2] << 8) | ip[3];

        if (ihl < 20 || total <= ihl || l2 + total > len) {
            return;
        }
        cso = l2 + ihl;
        l4len = total - ihl;
    } else if ((ip[0] >> 4) == 6) {
        unsigned plen;

        if (len < l2 + 40) {
            return;
        }
        plen = ((unsigned)ip[4] << 8) | ip[5];
        if (l2 + 40 + plen > len) {
            return;
        }
        cso = l2 + 40;
        l4len = plen;
    } else {
        return;
    }
    if (cso + 4 > len || off + 2 > len || off < cso) {
        return;
    }

    sum = 0;
    if ((ip[0] >> 4) == 4) {
        /* src + dst + protocol + L4 length */
        sum = edma_csum_add(sum, ip + 12, 8);
        sum += ip[9];
        sum += (uint32_t)l4len;
    } else {
        uint32_t vlen = (uint32_t)l4len;

        /* src + dst + 32-bit L4 length + next header */
        sum = edma_csum_add(sum, ip + 8, 32);
        sum += (vlen >> 16) + (vlen & 0xffff);
        sum += ip[6];
    }

    f[off] = f[off + 1] = 0;
    sum = edma_csum_add(sum, f + cso, l4len);
    csum = ~edma_csum_fold(sum);
    /*
     * A UDP checksum of 0 means "no checksum", so 0xffff is transmitted
     * instead whenever the computed value comes out as zero.
     */
    if (csum == 0 && (word1 & EDMA_TPD_CUSTOM_CSUM) == EDMA_TPD_UDP_CSUM) {
        csum = 0xffff;
    }
    f[off] = csum >> 8;
    f[off + 1] = csum & 0xff;
    EDMA_DBG("tx csum: cso=%zu off=%zu l4len=%zu -> %04x\n",
             cso, off, l4len, csum);
}

static void edma_tx_send(IPQEdma *e, const uint8_t *f, size_t len,
                         uint32_t word3)
{
    uint32_t ports;
    ssize_t ret;

    if (!e->nc || !len) {
        return;
    }
    ports = (word3 >> EDMA_TPD_PORT_SHIFT) & 0x7f;
    if (!(ports & (1u << e->bridge_port))) {
        /*
         * The switch would only put this frame on the ports in the bitmap.
         * A bridged LAN floods to every front-panel port, so this normally
         * means the frame was steered somewhere we do not model.
         */
        if (e->tx_drop_noport < 8) {
            EDMA_DBG("tx: drop %zuB bitmap %02x excludes bridge port %d"
                     " (ethertype %02x%02x)\n", len, ports, e->bridge_port,
                     len > 13 ? f[12] : 0, len > 13 ? f[13] : 0);
        }
        e->tx_drop_noport++;
        return;
    }
    if (e->nc->link_down || !e->nc->peer) {
        if (!e->tx_drop_nolink) {
            EDMA_DBG("tx: netdev '%s' down (link_down=%d peer=%p)\n",
                     e->nc->name, e->nc->link_down, (void *)e->nc->peer);
        }
        e->tx_drop_nolink++;
        return;
    }
    NET_TRACE("tx %zuB\n", len);
    ret = qemu_send_packet(e->nc, f, (int)len);
    if (ret < 0 && !e->tx_drop_nolink) {
        EDMA_DBG("tx: netdev '%s' refused a %zu byte frame (%zd)\n",
                 e->nc->name, len, ret);
    }
    e->tx_host++;
    if (!(e->tx_host % 500)) {
        EDMA_DBG("net: tx=%" PRIu64 " (noport=%" PRIu64 " nolink=%" PRIu64
                 ") rx=%" PRIu64 " (dropped=%" PRIu64 ")\n",
                 e->tx_host, e->tx_drop_noport, e->tx_drop_nolink,
                 e->rx_host, e->rx_dropped);
    }
}

/*
 * TCP segmentation offload.  The guest hands the engine one super-packet and
 * expects it to emit MTU-sized segments with the lengths and checksums fixed
 * up; without this a jumbo TCP send would reach the tap as a single frame
 * that the host would have to drop.
 */
static int edma_tx_lso(IPQEdma *e, const uint8_t *frame, size_t len,
                       uint32_t word1, uint32_t word3)
{
    unsigned l2 = edma_l2_len(frame, len);
    const uint8_t *ip, *tcp;
    unsigned mss = (word1 >> EDMA_TPD_CSUM_OFF_SHIFT) & 0x1fff;
    unsigned ihl, thl, hdr;
    size_t plen, done;
    uint32_t seq;
    int v6;

    if (!mss || len < l2 + 40) {
        return -1;
    }
    ip = frame + l2;
    v6 = (ip[0] >> 4) == 6;
    if (!v6 && (ip[0] >> 4) != 4) {
        return -1;
    }
    ihl = v6 ? 40 : (unsigned)(ip[0] & 0x0f) * 4;
    if ((v6 ? ip[6] : ip[9]) != 6 /* TCP */ || len < l2 + ihl + 20) {
        return -1;
    }
    tcp = ip + ihl;
    thl = (tcp[12] >> 4) * 4;
    if (thl < 20) {
        return -1;
    }
    hdr = l2 + ihl + thl;
    if (hdr > len) {
        return -1;
    }
    plen = len - hdr;
    seq = ((uint32_t)tcp[4] << 24) | ((uint32_t)tcp[5] << 16) |
          ((uint32_t)tcp[6] << 8) | tcp[7];

    for (done = 0; done < plen; done += mss) {
        size_t seg = MIN((size_t)mss, plen - done);
        uint8_t *out = e->segbuf;
        uint8_t *oip = out + l2;
        uint8_t *otcp = oip + ihl;
        uint32_t sum = 0;
        uint16_t csum;

        memcpy(out, frame, hdr);
        memcpy(out + hdr, frame + hdr + done, seg);

        if (v6) {
            unsigned ulen = thl + seg;

            oip[4] = ulen >> 8;
            oip[5] = ulen & 0xff;
            sum = edma_csum_add(sum, oip + 8, 32);      /* src + dst */
            sum += ulen >> 16;
            sum += ulen & 0xffff;
            sum += 6;
        } else {
            unsigned total = ihl + thl + seg;

            oip[2] = total >> 8;
            oip[3] = total & 0xff;
            oip[10] = oip[11] = 0;
            csum = ~edma_csum_fold(edma_csum_add(0, oip, ihl));
            oip[10] = csum >> 8;
            oip[11] = csum & 0xff;
            sum += edma_csum_add(0, oip + 12, 8);       /* src + dst */
            sum += 6;
            sum += thl + seg;
        }

        seq += done;
        otcp[4] = seq >> 24;
        otcp[5] = seq >> 16;
        otcp[6] = seq >> 8;
        otcp[7] = seq;
        seq -= done;
        otcp[16] = otcp[17] = 0;
        sum = edma_csum_add(sum, otcp, thl + seg);
        csum = ~edma_csum_fold(sum);
        otcp[16] = csum >> 8;
        otcp[17] = csum & 0xff;

        edma_tx_send(e, out, hdr + seg, word3);
    }
    return 0;
}

/* Hand one assembled frame to the engine's egress path. */
static void edma_tx_frame(IPQEdma *e, uint8_t *f, size_t len, uint32_t word1,
                          uint32_t word3)
{
    if (!len) {
        return;
    }
    if (word1 & EDMA_TPD_LSO) {
        if (edma_tx_lso(e, f, len, word1, word3) < 0) {
            EDMA_DBG("tx: cannot offload lso word1=%08x, dropped\n", word1);
        }
        return;
    }
    if ((word1 & EDMA_TPD_CUSTOM_CSUM) == EDMA_TPD_CUSTOM_CSUM) {
        edma_tx_csum(f, len, word1);
    }
    if (env_u32("QEMU_IPQ_EDMA_LOG", 0) > 2) {
        unsigned n = MIN(len, 32u), i;

        fprintf(stderr, "edma: tx %zuB word1=%08x:", len, word1);
        for (i = 0; i < n; i++) {
            fprintf(stderr, " %02x", f[i]);
        }
        fprintf(stderr, "\n");
    }
    edma_tx_send(e, f, len, word3);
}

/*
 * Number of RFD buffers the driver has published but not consumed yet.  The
 * producer index names the newest buffer the driver filled, so the slots the
 * engine may still use are cons..prod-1 and the one at prod is the guard.
 */
static unsigned edma_rx_free(IPQEdma *e, unsigned q, unsigned *prod_out,
                             unsigned *cons_out, unsigned *ring_out,
                             uint32_t *base_out)
{
    uint32_t idx = e->regs[EDMA_REG_RFD_IDX_Q(q) / 4];
    uint32_t desc0 = e->regs[EDMA_REG_RX_DESC0 / 4];
    unsigned prod = idx & EDMA_RFD_PROD_MASK;
    unsigned cons = (idx >> EDMA_RFD_CONS_SHIFT) & EDMA_RFD_PROD_MASK;
    unsigned ring = desc0 & 0xfffu;
    uint32_t base = e->regs[EDMA_REG_RFD_BASE_Q(q) / 4];

    if (!ring) {
        ring = EDMA_RX_RING_SIZE;
    }
    if (prod >= ring) {
        prod %= ring;
    }
    if (cons >= ring) {
        cons %= ring;
    }
    if (prod_out) {
        *prod_out = prod;
    }
    if (cons_out) {
        *cons_out = cons;
    }
    if (ring_out) {
        *ring_out = ring;
    }
    if (base_out) {
        *base_out = base;
    }
    return (prod - cons) & (ring - 1);
}

static unsigned edma_rx_bufs_needed(size_t len)
{
    unsigned n = 1;
    size_t rest = len;

    if (rest <= EDMA_RX_HEAD_BUFF_SIZE - EDMA_RRD_SIZE) {
        return 1;
    }
    rest -= EDMA_RX_HEAD_BUFF_SIZE - EDMA_RRD_SIZE;
    n += DIV_ROUND_UP(rest, EDMA_RX_HEAD_BUFF_SIZE);
    return n;
}

/*
 * Put one received frame into the driver's RX ring.  The engine writes a
 * 16-byte RRD in front of the frame in the first buffer and raw bytes in the
 * buffers after it, then advances the consumer index -- the driver's poll
 * compares that index against its own tail and takes everything in between.
 */
static void edma_rx_deliver(IPQEdma *e, const uint8_t *frame, size_t len)
{
    /* The driver gives queue 0 ring index 0, queue 1 index 2, and so on. */
    const unsigned q = 0;
    unsigned prod, cons, ring, nbuf, i;
    uint32_t base;
    unsigned free;
    size_t off = 0;

    if (!len || len > EDMA_MAX_FRAME) {
        e->rx_dropped++;
        return;
    }
    free = edma_rx_free(e, q, &prod, &cons, &ring, &base);
    nbuf = edma_rx_bufs_needed(len);
    if (!base || free < nbuf) {
        e->rx_dropped++;
        return;
    }

    for (i = 0; i < nbuf; i++) {
        unsigned slot = (cons + i) % ring;
        unsigned cap = (i == 0) ? EDMA_RX_HEAD_BUFF_SIZE - EDMA_RRD_SIZE
                                : EDMA_RX_HEAD_BUFF_SIZE;
        size_t n = MIN((size_t)cap, len - off);
        uint32_t addr = 0;

        dma_memory_read(&address_space_memory,
                        (hwaddr)base + (hwaddr)slot * 4, &addr, 4,
                        MEMTXATTRS_UNSPECIFIED);
        addr = le32_to_cpu(addr);
        if (!addr) {
            e->rx_dropped++;
            return;
        }
        if (i == 0) {
            uint16_t rrd[EDMA_RRD_SIZE / 2] = { 0 };

            rrd[1] = cpu_to_le16(nbuf | (e->bridge_port << 12));
            rrd[6] = cpu_to_le16(len & 0x3fff);
            rrd[7] = cpu_to_le16(EDMA_RRD_VALID);
            dma_memory_write(&address_space_memory, addr, rrd, sizeof(rrd),
                             MEMTXATTRS_UNSPECIFIED);
        }
        dma_memory_write(&address_space_memory,
                         (hwaddr)addr + (i ? 0 : EDMA_RRD_SIZE),
                         frame + off, n, MEMTXATTRS_UNSPECIFIED);
        off += n;
    }

    EDMA_DBG("rx q=%u port=%d len=%zu nbuf=%u slot=%u cons %u->%u prod=%u\n",
             q, e->bridge_port, len, nbuf, (cons + ring - nbuf) % ring,
             (cons + ring - nbuf) % ring, (cons + nbuf) % ring, prod);
    cons = (cons + nbuf) % ring;
    e->regs[EDMA_REG_RFD_IDX_Q(q) / 4] =
        (cons << EDMA_RFD_CONS_SHIFT) | prod;
    e->regs[EDMA_REG_RX_ISR / 4] |= 1u << q;
    edma_update_irqs(e);
    e->rx_host++;
    if (!(e->rx_host % 500)) {
        EDMA_DBG("net: tx=%" PRIu64 " (noport=%" PRIu64 " nolink=%" PRIu64
                 ") rx=%" PRIu64 " (dropped=%" PRIu64 ")\n",
                 e->tx_host, e->tx_drop_noport, e->tx_drop_nolink,
                 e->rx_host, e->rx_dropped);
    }
}

/* Walk the descriptors the driver just published, retire them and forward. */
static void edma_tx_kick(IPQEdma *e, unsigned q)
{
    uint32_t idx = e->regs[EDMA_REG_TPD_IDX_Q(q) / 4];
    uint32_t prod = idx & EDMA_TPD_PROD_MASK;
    uint32_t cons = (idx >> EDMA_TPD_CONS_SHIFT) & EDMA_TPD_PROD_MASK;
    uint32_t ring = e->regs[EDMA_REG_TPD_RING_SIZE / 4];
    uint32_t base = e->regs[EDMA_REG_TPD_BASE_Q(q) / 4];
    uint32_t n;
    size_t flen = 0;
    unsigned i;

    if (!ring) {
        ring = 128;
    }
    /*
     * Both indices are ring-relative: the driver advances them with
     * IPQESS_NEXT_IDX() == ((i + 1) & (count - 1)), so going from count-1
     * back to 0 is one step, not count-1 of them.  Measuring the distance
     * with the 16-bit field mask would turn every wrap into a full ring of
     * stale descriptors, which resends old frames and desynchronises the
     * driver's tail.
     */
    if (prod >= ring || cons >= ring) {
        EDMA_DBG("kick q=%u: index outside ring (prod=%u cons=%u ring=%u)\n",
                 q, prod, cons, ring);
        prod %= ring;
        cons %= ring;
    }
    n = (prod + ring - cons) % ring;
    EDMA_DBG("kick q=%u prod=%u cons=%u n=%u base=%08x\n",
             q, prod, cons, n, base);
    if (!n) {
        return;
    }

    for (i = 0; i < n; i++) {
        uint8_t tpd[EDMA_TPD_SIZE];
        uint32_t word1, word3, addr;
        uint16_t dlen;

        if (!base) {
            cons = (cons + 1) % ring;
            continue;
        }
        dma_memory_read(&address_space_memory,
                        (hwaddr)base + (hwaddr)cons * EDMA_TPD_SIZE,
                        tpd, sizeof(tpd), MEMTXATTRS_UNSPECIFIED);
        dlen = lduw_le_p(tpd + 0);
        word1 = ldl_le_p(tpd + 4);
        addr = ldl_le_p(tpd + 8);
        word3 = ldl_le_p(tpd + 12);

        if (env_u32("QEMU_IPQ_EDMA_LOG", 0) > 1) {
            uint8_t peek[64];

            edma_hexdump("tpd", tpd, sizeof(tpd));
            if (addr >= 0x80000000u && addr < 0x90000000u &&
                dlen >= 16) {
                dma_memory_read(&address_space_memory, addr, peek, 48,
                                MEMTXATTRS_UNSPECIFIED);
                edma_hexdump("frame", peek, 48);
            }
        }

        EDMA_DBG("tpd q=%u slot=%u len=%u addr=%08x word1=%08x word3=%08x\n",
                 q, cons, dlen, addr, word1, word3);
        e->tx_byte[q] += dlen;
        if (dlen && flen + dlen <= EDMA_MAX_FRAME &&
            addr >= 0x80000000u && addr < 0x90000000u) {
            dma_memory_read(&address_space_memory, addr, e->txbuf + flen,
                            dlen, MEMTXATTRS_UNSPECIFIED);
            flen += dlen;
        }
        if (word1 & EDMA_TPD_EOP) {
            e->tx_pkt[q]++;
            edma_tx_frame(e, e->txbuf, flen, word1, word3);
            flen = 0;
        }
        cons = (cons + 1) % ring;
    }

    e->regs[EDMA_REG_TPD_IDX_Q(q) / 4] = (cons << EDMA_TPD_CONS_SHIFT) | prod;
    e->regs[EDMA_REG_TX_ISR / 4] |= 1u << q;
    edma_update_irqs(e);
}

/* ------------------------------------------------------------------ */
/* The host side: one QEMU net client, one bridged switch port         */
/* ------------------------------------------------------------------ */

/*
 * The engine's net client.  It is created with qemu_new_nic(), not
 * qemu_new_net_client(): QEMU's packet delivery path calls qemu_get_nic() on
 * every client whose type is NET_CLIENT_DRIVER_NIC, and that helper recovers
 * the NICState by subtracting info->size from the NetClientState -- which is
 * only correct for the layout qemu_new_nic() allocates.  A plain
 * qemu_new_net_client() client has no NICState in front of it, so the very
 * first received frame dereferenced a bogus reentrancy_guard and QEMU died.
 *
 * info->size is therefore sizeof(NICState): the per-queue state we need lives
 * in IPQEdma, and the client is reached through qemu_get_queue(nic).
 */
static ssize_t edma_net_receive(NetClientState *nc, const uint8_t *buf,
                                size_t size)
{
    IPQEdma *e = qemu_get_nic_opaque(nc);

    NET_TRACE("rx %zuB\n", size);
    edma_rx_deliver(e, buf, size);
    return size;
}

static bool edma_net_can_receive(NetClientState *nc)
{
    IPQEdma *e = qemu_get_nic_opaque(nc);

    /* Keep room for two full-size frames so a burst is not dropped. */
    return edma_rx_free(e, 0, NULL, NULL, NULL, NULL) >= 2;
}

static void edma_net_link_status(NetClientState *nc)
{
    IPQEdma *e = qemu_get_nic_opaque(nc);

    ipq_eth_set_host_link(e->bridge_port, !nc->link_down);
}

static NetClientInfo edma_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = edma_net_receive,
    .can_receive = edma_net_can_receive,
    .link_status_changed = edma_net_link_status,
};

/*
 * Attach the engine to a QEMU network backend.  QEMU_IPQ_NETDEV names the
 * -netdev (default "net0") and QEMU_IPQ_ETH_BRIDGE_PORT the switch port wired
 * to it (default 5, the front-panel port the DTS calls lan1).  Without a
 * matching -netdev the machine still boots and simply drops every frame, the
 * way it did before there was a packet path at all.
 */
static void edma_connect_netdev(IPQEdma *e)
{
    const char *id = getenv("QEMU_IPQ_NETDEV");
    const char *bridge = getenv("QEMU_IPQ_ETH_BRIDGE_PORT");
    NetClientState *peer;
    NICState *nic;

    e->bridge_port = bridge ? atoi(bridge) : 5;
    if (e->bridge_port < 0 || e->bridge_port > 6) {
        warn_report("ipq4019: bad QEMU_IPQ_ETH_BRIDGE_PORT '%s'", bridge);
        e->bridge_port = 5;
    }
    e->txbuf = g_malloc(EDMA_MAX_FRAME);
    e->segbuf = g_malloc(EDMA_MAX_FRAME);

    if (!id) {
        id = "net0";
    }
    peer = qemu_find_netdev(id);
    if (!peer) {
        if (getenv("QEMU_IPQ_NETDEV")) {
            warn_report("ipq4019: no -netdev with id '%s'; Ethernet frames "
                        "are dropped", id);
        }
        return;
    }
    e->nic.peers.ncs[0] = peer;
    e->nic.peers.queues = 1;
    nic = qemu_new_nic(&edma_net_info, &e->nic, "ipqess", "eth0",
                       &e->rx_guard, e);
    e->nc = qemu_get_queue(nic);
    e->host_link = !peer->link_down;
    ipq_eth_set_host_link(e->bridge_port, e->host_link);
    info_report("ipq4019: eth0 -> -netdev '%s' on switch port %d (%s)",
                id, e->bridge_port, ipq_port_name(e->bridge_port));
}

static uint64_t edma_read(void *opaque, hwaddr off, unsigned size)
{
    IPQEdma *e = opaque;
    unsigned q;

    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        if (off == EDMA_REG_TX_STAT_PKT_Q(q)) {
            return e->tx_pkt[q];
        }
        if (off == EDMA_REG_TX_STAT_BYTE_Q(q)) {
            return e->tx_byte[q];
        }
    }
    return e->regs[off / 4];
}

static void edma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQEdma *e = opaque;
    unsigned q;

    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        if (off == EDMA_REG_TPD_IDX_Q(q)) {
            /*
             * The driver publishes the producer index with a plain 32-bit
             * write (ipqess_kick_tx() writes tx_ring->head), so the upper
             * half of the value is *not* the consumer index -- the engine
             * owns that half and keeps it across the write.  Storing the
             * value verbatim reset the consumer to 0 on every kick and made
             * the engine re-walk descriptor slots the driver had already
             * freed and reused, which put pages of unrelated guest memory on
             * the wire as 70-byte "frames".
             */
            e->regs[off / 4] = (e->regs[off / 4] & 0xffff0000u) |
                               ((uint32_t)val & EDMA_TPD_PROD_MASK);
            edma_tx_kick(e, q);
            return;
        }
    }

    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        if (off == EDMA_REG_RFD_IDX_Q(q)) {
            /*
             * Same split ownership as the TPD index: the driver publishes
             * receive buffers by writing the producer half, the engine owns
             * the consumer half.  Storing the value verbatim would reset the
             * engine's consumer to 0 and make it overwrite buffers the driver
             * had not taken yet.
             */
            e->regs[off / 4] = (e->regs[off / 4] & ~EDMA_RFD_PROD_MASK) |
                               ((uint32_t)val & EDMA_RFD_PROD_MASK);
            /*
             * The driver just made room, so tell QEMU to look again.  A net
             * client whose can_receive() said no gets its frames queued and
             * its backend read poll switched off -- for a tap that is the
             * host's side of the cable -- and nothing re-checks until the
             * device asks.  One frame that arrives while the ring is full
             * (the host's IPv6 DAD at boot, long before the guest's driver
             * publishes a single buffer) therefore wedged the receive path
             * for the whole session: the guest saw no input at all while its
             * own output worked.
             */
            if (e->nc) {
                qemu_flush_queued_packets(e->nc);
            }
            return;
        }
    }

    /*
     * Both interrupt-status registers are write-1-to-clear.  This matters a
     * lot for RX: ipqess_irq_enable() and ipqess_rx_napi() clear the pending
     * bits by writing ones, and ipqess_rx_napi() loops
     *
     *     do { ...; status = read(RX_ISR); } while (budget && (status & mask));
     *
     * so a register that latched the write kept the driver spinning in that
     * loop forever, with interrupts off -- the machine looked frozen right
     * after the link came up.
     */
    if (off == EDMA_REG_TX_ISR || off == EDMA_REG_RX_ISR) {
        EDMA_DBG("%s_isr clear %08x -> %08x\n",
                 off == EDMA_REG_TX_ISR ? "tx" : "rx", (uint32_t)val,
                 e->regs[off / 4] & ~(uint32_t)val);
        e->regs[off / 4] &= ~(uint32_t)val;
        edma_update_irqs(e);
        return;
    }

    e->regs[off / 4] = (uint32_t)val;
    if (off >= EDMA_REG_RX_INT_MASK_Q(0) && off <= EDMA_REG_TX_INT_MASK_Q(15)) {
        EDMA_DBG("mask write off=%03x val=%08x\n", (unsigned)off,
                 (uint32_t)val);
    }
    edma_update_irqs(e);
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static IPQEdma *add_edma(MemoryRegion *parent, hwaddr base, hwaddr size)
{
    IPQEdma *e = g_new0(IPQEdma, 1);

    e->nregs = size / 4;
    e->regs = g_new0(uint32_t, e->nregs);
    memory_region_init_io(&e->mr, NULL, &edma_ops, e, "edma", size);
    memory_region_add_subregion(parent, base, &e->mr);
    ipq_edma = e;
    return e;
}

/* TX queue q is <GIC_SPI 0x41 + q>, RX queue q <GIC_SPI 0xf0 + q>. */
static void edma_wire_irqs(DeviceState *gicdev)
{
    unsigned q;

    if (!ipq_edma || !gicdev) {
        return;
    }
    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        ipq_edma->irq_tx[q] = qdev_get_gpio_in(gicdev, 0x41 + q);
        ipq_edma->irq_rx[q] = qdev_get_gpio_in(gicdev, 0xf0 + q);
    }
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

static IPQMdio *ipq_mdio;        /* set by add_mdio(), reset with the SoC */

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
 * with the MAC in loopback sees as many good frames as it was asked to send,
 * which is the result the switch driver's PSGMII self test looks for.
 *
 * Control register bits: START = 0x2000, INPROGR = 0x8000.  The generator is
 * kicked by writing 0xa000 and the hardware clears INPROGR once the run is
 * over.  A model that kept INPROGR set made every boot sit through the
 * driver's "recalibrate up to 100 times" loop -- each pass waits a full
 * second per port for this bit, i.e. ~15 minutes of silence before userspace
 * started.  Clearing it here makes the self test pass on the first pass.
 */
#define QCA8075_PKT_GEN_CTRL        0x8020
#define QCA8075_PKT_GEN_PKT_NUMB    0x8021
#define QCA8075_PKT_GEN_START       0x2000
#define QCA8075_PKT_GEN_INPROGR     0x8000
#define QCA8075_PKT_GEN_DEFAULT     4096    /* driver's default packet count */

static void phy_mmd_pkt_gen_ctrl(IPQPhy *p, uint16_t val)
{
    if (val & QCA8075_PKT_GEN_START) {
        uint16_t n = mmd_get(p, 7, QCA8075_PKT_GEN_PKT_NUMB);

        if (!n) {
            n = QCA8075_PKT_GEN_DEFAULT;
        }
        mmd_set(p, 7, 0x802e, n);       /* tx ok, low 16 bits */
        mmd_set(p, 7, 0x802d, 0x0000);  /* tx ok, high 16 bits */
        mmd_set(p, 7, 0x802f, 0x0000);  /* tx errors */
        mmd_set(p, 7, 0x802b, n);       /* rx ok, low 16 bits */
        mmd_set(p, 7, 0x802a, 0x0000);  /* rx ok, high 16 bits */
        mmd_set(p, 7, 0x802c, 0x0000);  /* rx errors */
        val &= ~QCA8075_PKT_GEN_INPROGR;        /* the run completes at once */
    } else if (!val) {
        mmd_set(p, 7, 0x802e, 0);
        mmd_set(p, 7, 0x802d, 0);
        mmd_set(p, 7, 0x802f, 0);
        mmd_set(p, 7, 0x802b, 0);
        mmd_set(p, 7, 0x802a, 0);
        mmd_set(p, 7, 0x802c, 0);
    }
    mmd_set(p, 7, QCA8075_PKT_GEN_CTRL, val);
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
        return 0x0400 | 0x0800 |          /* link, speed/duplex resolved */
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
            if (p->mmd_dev == 7 && p->mmd_reg == QCA8075_PKT_GEN_CTRL) {
                phy_mmd_pkt_gen_ctrl(p, val);
            } else {
                mmd_set(p, p->mmd_dev, p->mmd_reg, val);
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
        /*
         * RESET (bit 15) and RESTART_AUTONEG (bit 9) are self-clearing on
         * silicon: the chip drops them as soon as the operation finishes.
         * Keeping bit 9 set is not harmless -- genphy_update_link() reads
         * BMCR first and, seeing "autonegotiation is being started",
         * reports the link down *without reading BMSR at all*.  That is
         * how every front-panel port stayed at carrier 0 while the PHY
         * model happily answered "link up" to everything else.
         */
        val &= ~0xa200u;                /* 0x8000 RESET | 0x0200 ANRESTART */

        if (val & 0x1000) {
            /*
             * Autonegotiation enabled: the resolved result comes from the
             * link partner advertisement (LPA 0x45e1) and the 1000BASE-T
             * status registers, both of which say 1000 Mb/s full duplex.
             */
            p->speed = 1000;
            p->full_duplex = true;
        } else if (val & 0x2000) {
            p->speed = 1000;
            p->full_duplex = (val & 0x0100) != 0;
        } else {
            p->speed = (val & 0x0040) ? 100 : 10;
            p->full_duplex = (val & 0x0100) != 0;
        }
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

/*
 * The switch port names come from the board's DTS (`ports/port@N/label`), and
 * the IPQ4019 reference design puts the CPU port at 0, wan on port 1 and the
 * four LAN ports on 2..5 in reverse order.  That is also the order DSA
 * registers them, and each user port N is wired to MDIO address N - 1.
 */
static const char *ipq_port_name(int port)
{
    static const char *const names[] = {
        "cpu", "wan", "lan4", "lan3", "lan2", "lan1",
    };

    if (port < 0 || port >= (int)ARRAY_SIZE(names)) {
        return "?";
    }
    return names[port];
}

/*
 * Reflect the host backend's carrier on the PHY of the bridged switch port.
 * A tap or bridge whose link is down should look like an unplugged cable, not
 * like a port that reports link up and silently eats every frame.
 */
static void ipq_eth_set_host_link(int switch_port, bool up)
{
    IPQPhy *p;

    if (!ipq_mdio || switch_port < 1) {
        return;
    }
    p = mdio_lookup(ipq_mdio, switch_port - 1);
    if (!p) {
        return;
    }
    if (p->link != up) {
        info_report("ipq4019: %s: host link %s",
                    ipq_port_name(switch_port), up ? "up" : "down");
    }
    p->link = up;
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
                qemu_log_mask(LOG_UNIMP, "mdio: %s phy %d reg %02x %s 0x%04x%s%s\n",
                              (val & MDIO_CMD_WRITE) ? "wr" : "rd", phy_addr, reg,
                              (val & MDIO_CMD_WRITE) ? "<-" : "->",
                              (val & MDIO_CMD_WRITE) ? (m->wdata & 0xffff) : m->rdata,
                              trace_ms(),
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
        fprintf(stderr, "ipq4019: dtb port %d", reg ? (int)fdt32_to_cpu(reg[0]) : -1);
        if (label) {
            fprintf(stderr, " \"%s\"", label);
        }
        if (phy == -2) {
            fprintf(stderr, " fixed-link\n");
        } else if (phy >= 0) {
            fprintf(stderr, " -> mdio phy %d\n", phy);
        } else {
            fprintf(stderr, " -> disconnected\n");
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
        fprintf(stderr, "ipq4019: %d mdio port(s) from %s\n", count, path);
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
    static char *consumed;
    const char *path = getenv("QEMU_IPQ_DTB");

    if (path) {
        return path;
    }
    if (machine->dtb) {
        /*
         * Consume it: this firmware has its own DTB in flash, and letting
         * arm_load_kernel() copy the description into guest RAM would land
         * on top of the image the boot loader is about to use.
         */
        consumed = g_strdup(machine->dtb);
        g_free(machine->dtb);
        machine->dtb = NULL;
        return consumed;
    }
    return NULL;
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

    ipq_mdio = m;
    memory_region_init_io(&m->mr, NULL, &mdio_ops, m, "mdio", IPQ_MDIO_SIZE);
    memory_region_add_subregion(parent, IPQ_MDIO_BASE, &m->mr);

    fprintf(stderr, "ipq4019: mdio@%08x, %d port(s):", IPQ_MDIO_BASE, m->nphy);
    for (i = 0; i < m->nphy; i++) {
        fprintf(stderr, " %d@%d%s", m->phy[i].addr, m->phy[i].speed,
               m->phy[i].link ? "" : "(down)");
    }
    fprintf(stderr, "\n");
    return m;
}

/* ------------------------------------------------------------------ */
/* BLSP UARTDM                                                         */
/* ------------------------------------------------------------------ */

/*
 * The console UART, as the kernel's msm_serial driver sees it: UARTDM
 * register file, a 64-byte RX FIFO fed by the chardev, and the shared
 * ISR/IMR register pair driving one GIC line.
 *
 * Two details matter for a working console:
 *
 *  - 0x10 reads as the *masked* interrupt status (MISR) and writes as the
 *    command register (CR); 0x14 reads as ISR and writes as IMR.  The
 *    driver's whole tty output path hangs off this: uart_start_tx() only
 *    enables TXLEV, and the bytes are written by msm_handle_tx() from the
 *    interrupt handler.  With no interrupt userspace writes to
 *    /dev/console produce nothing at all (preinit's messages never
 *    appeared).
 *
 *  - 0x08 (0xa4 in the boot firmware's numbering) reports TX_READY and
 *    TX_EMPTY at all times -- the model drains the FIFO synchronously --
 *    plus RX_READY/RX_FULL when input is queued.
 *
 * RX word/partial-word handling mirrors the hardware closely enough for
 * the driver's two paths: whole words raise RXLEV (bit 4) and are counted
 * in RFWR/RXFS; a partial word raises RXSTALE (bit 3) after the stale
 * timeout and is counted by RX_TOTAL_SNAP.
 */
typedef struct IPQUart {
    MemoryRegion mr;
    CharFrontend chr;
    const char *name;
    qemu_irq irq;

    uint32_t mr1;
    uint32_t mr2;
    uint32_t imr;               /* written at 0x14 */
    uint32_t cr;                /* last command written at 0x10 */
    uint32_t tfwr;
    uint32_t rfwr;
    uint32_t dmrx;
    uint32_t dmen;
    uint32_t ncf_tx;

    uint8_t rxbuf[64];
    unsigned rx_head;
    unsigned rx_count;
    /*
     * Bytes the chardev has handed us but that have not reached the receive
     * FIFO yet.  A chardev write is a burst -- a pasted line, a whole pty
     * read -- while the die delivers one byte at a time at the baud rate, so
     * queueing here is what keeps a burst longer than the 64-byte FIFO from
     * being silently truncated at the FIFO boundary.
     */
    uint8_t rxq[512];
    unsigned rxq_head;
    unsigned rxq_count;
    bool in_fill;
    bool rx_again;
    /*
     * RX_TOTAL_SNAP counts the bytes the BAM receiver has written into the
     * guest's DMA buffer since the buffer was posted, not the bytes sitting
     * in the FIFO: msm_complete_rx_dma() takes `snap - old_snap_state` and
     * feeds exactly that many bytes of the buffer to the tty.  Counting
     * arrived-but-not-moved bytes makes it hand the tty stale bytes and drop
     * the ones that arrive next (visible as characters vanishing out of the
     * middle of a typed line).
     */
    uint32_t rx_total;          /* bytes moved to DRAM since DMRX/read */
    bool rx_stale;              /* partial word waiting for the timeout */
    QEMUTimer *stale_timer;
} IPQUart;

/* The console UART, so the BAM UART TX pipe can drive the same backend. */
static IPQUart *ipq_console_uart;

/* Set when the machine instantiates the GIC, so devices added later can
 * claim their SPI input line. */
static DeviceState *ipq_gic_device;

#define UART_RX_FIFO_SIZE   (sizeof(((IPQUart *)0)->rxbuf))

/* Stale (partial word) timeout, in microseconds of virtual time. */
#define UART_RX_STALE_US    1000

/*
 * UARTDM_DMEN bit 3: the receiver is drained by the BAM instead of the
 * FIFO/watermark interrupt path (UARTDM v1.4).
 */
#define UART_DMEN_RX_BAM_ENABLE (1u << 3)


static uint32_t uart_isr(IPQUart *u)
{
    uint32_t isr = 0;

    /* The TX FIFO is always drained by the time software looks at it. */
    isr |= 0x01;                    /* TXLEV */
    /*
     * RX_READY.  Both bootloaders poll this one bit, and nothing else, before
     * reading a character:
     *
     *   RouterBOOT  ldr r0,[base+0x10] / tst r0,#8  / beq back
     *   U-Boot      ldr r3,[r2,#0x10] / tst r3,#8  / bne have-data
     *
     * and when the bit is clear they never look at the FIFO again -- a key
     * press at the "press any key" window or at the U-Boot prompt just stops
     * the boot dead.  Linux's interrupt handler tests bits 3 and 4 together
     * (`tst r6,#0x18`, then services receive), so raising bit 3 for "a byte is
     * buffered" is what all three consumers expect.  Bit 4 is the level flag
     * it has always been.
     */
    if (u->rx_count || u->rx_stale) {
        isr |= 0x08;                /* RX data available (or a stale word) */
    }
    if (u->rx_count >= 4) {
        isr |= 0x10;                /* RXLEV */
    }
    return isr;
}

static void uart_update_irq(IPQUart *u)
{
    if (u->irq) {
        uint32_t isr = uart_isr(u);
        int level = (isr & u->imr) != 0;

        /*
         * This runs from the chardev receive callback too, i.e. outside any
         * CPU context, so the PC has to come from trace_pc() (which copes
         * with current_cpu == NULL).  Dereferencing current_cpu directly here
         * crashed QEMU the moment the first key arrived.
         */
        UART_RX_LOG("%s irq level=%d isr=%02x imr=%02x rx_count=%u%s\n",
                    u->name, level, isr, u->imr, u->rx_count, trace_pc());
        qemu_set_irq(u->irq, level);
    }
}

static void uart_stale_expire(void *opaque)
{
    IPQUart *u = opaque;

    UART_RX_LOG("%s stale timer: rx_count=%u dmen=%x\n", u->name,
                u->rx_count, u->dmen);
    /*
     * In DMA mode the hardware flushes its receive FIFO to the BAM after the
     * idle timeout; that flush is what completes the driver's receive
     * descriptor.  No FIFO interrupt is raised -- RXLEV/RXSTALE are masked by
     * the driver while DMA is in use.  This runs even with an empty FIFO: the
     * bytes are already inside the descriptor the engine is filling, and the
     * idle timeout is what retires it.
     */
    if (u->dmen & UART_DMEN_RX_BAM_ENABLE) {
        bam_uart_rx_idle();
        return;
    }
    if (!u->rx_count) {
        return;
    }
    u->rx_stale = true;
    uart_update_irq(u);
}

static void uart_rx_fill(IPQUart *u)
{
    bool moved = false;

    if (!u) {
        return;
    }
    if (u->in_fill) {
        u->rx_again = true;             /* called back from the BAM move */
        return;
    }
    u->in_fill = true;

again:
    u->rx_again = false;
    while (u->rxq_count && u->rx_count < UART_RX_FIFO_SIZE) {
        u->rxbuf[(u->rx_head + u->rx_count) % UART_RX_FIFO_SIZE] =
            u->rxq[u->rxq_head];
        u->rxq_head = (u->rxq_head + 1) % (unsigned)sizeof(u->rxq);
        u->rxq_count--;
        u->rx_count++;
        moved = true;
    }
    if (moved) {
        if (u->rx_count % 4 || (u->dmen & UART_DMEN_RX_BAM_ENABLE)) {
            /*
             * A partial word can only be delivered through RXSTALE, and in
             * DMA mode the idle timeout is what flushes the last batch to
             * memory.
             */
            timer_mod(u->stale_timer,
                      qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + UART_RX_STALE_US);
        }
        uart_update_irq(u);
        if (u->dmen & UART_DMEN_RX_BAM_ENABLE) {
            /*
             * The BAM may have drained the FIFO into a posted descriptor,
             * which leaves room for the rest of the queued burst.
             */
            bam_uart_rx_kick();
        }
    }
    if (u->rx_again && u->rxq_count && u->rx_count < UART_RX_FIFO_SIZE) {
        moved = false;
        goto again;
    }
    u->in_fill = false;
}

static void uart_rx_room(IPQUart *u);

/* Called by the BAM when the driver posts a receive buffer. */
static void bam_uart_rx_input_ready(void)
{
    uart_rx_fill(ipq_console_uart);
}

/* Run the idle timeout that retires a partially filled receive descriptor. */
static void uart_rx_idle_arm(void)
{
    IPQUart *u = ipq_console_uart;

    if (u) {
        timer_mod(u->stale_timer,
                  qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) + UART_RX_STALE_US);
    }
}

static void uart_rx_pop_byte(IPQUart *u, uint8_t *out)
{
    if (u->rx_count) {
        *out = u->rxbuf[u->rx_head];
        u->rx_head = (u->rx_head + 1) % UART_RX_FIFO_SIZE;
        u->rx_count--;
    } else {
        *out = 0;
    }
    if (u->rx_count == 0 || u->rx_count >= 4) {
        u->rx_stale = false;
        timer_del(u->stale_timer);
    }
    uart_update_irq(u);
    uart_rx_fill(u);
    uart_rx_room(u);
}

static void uart_rx_pop(IPQUart *u, uint8_t *out)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        if (u->rx_count) {
            out[i] = u->rxbuf[u->rx_head];
            u->rx_head = (u->rx_head + 1) % UART_RX_FIFO_SIZE;
            u->rx_count--;
        } else {
            out[i] = 0;
        }
    }
    if (u->rx_count == 0 || u->rx_count >= 4) {
        u->rx_stale = false;
        timer_del(u->stale_timer);
    }
    uart_update_irq(u);
    uart_rx_fill(u);
    uart_rx_room(u);
}

static int uart_can_receive(void *opaque)
{
    IPQUart *u = opaque;

    /*
     * Report the whole queue, not the FIFO: the FIFO is drained by the BAM as
     * the guest consumes, and limiting the chardev to 64 bytes per read is
     * what used to cut long command lines in half.
     */
    return (int)sizeof(u->rxq) - (int)u->rxq_count;
}

/*
 * Tell the chardev there is room again.
 *
 * The chardev backend reads at most can_receive() bytes per read (the pty and
 * stdio backends size their read from qemu_chr_be_can_write()), and it only
 * asks again after the frontend calls qemu_chr_fe_accept_input().  Without
 * this the backend stops reading the moment the 64-byte receive FIFO fills,
 * so a burst longer than 64 bytes -- one pasted command line -- is truncated
 * exactly at the FIFO boundary and the rest never arrives.
 */
static void uart_rx_room(IPQUart *u)
{
    if (u->rx_count < UART_RX_FIFO_SIZE) {
        qemu_chr_fe_accept_input(&u->chr);
    }
}

/* ---- BAM receive side (see the BAM section for the caller) ---- */

static bool uart_bam_rx_armed(void)
{
    return ipq_console_uart &&
           (ipq_console_uart->dmen & UART_DMEN_RX_BAM_ENABLE);
}

static size_t uart_bam_rx_pending(void)
{
    return ipq_console_uart ? ipq_console_uart->rx_count : 0;
}

static size_t uart_bam_rx_pull(uint8_t *buf, size_t n)
{
    IPQUart *u = ipq_console_uart;
    size_t i;

    if (!u) {
        return 0;
    }
    for (i = 0; i < n && u->rx_count; i++) {
        buf[i] = u->rxbuf[u->rx_head];
        u->rx_head = (u->rx_head + 1) % UART_RX_FIFO_SIZE;
        u->rx_count--;
    }
    if (!u->rx_count) {
        u->rx_stale = false;
        timer_del(u->stale_timer);
    }
    /* The BAM just emptied (part of) the FIFO: top it up from the queue and
     * let the host hand us more. */
    uart_rx_fill(u);
    uart_rx_room(u);
    return i;
}

/* Bytes the BAM receiver just wrote into the posted descriptor. */
static void uart_bam_rx_account(size_t n)
{
    if (ipq_console_uart) {
        ipq_console_uart->rx_total += n;
    }
}

static void uart_receive(void *opaque, const uint8_t *buf, int size)
{
    IPQUart *u = opaque;
    int i;

    UART_RX_LOG("%s chardev delivered %d byte(s), first=%02x\n",
                u->name, size, size ? buf[0] : 0);
    for (i = 0; i < size && u->rxq_count < sizeof(u->rxq); i++) {
        u->rxq[(u->rxq_head + u->rxq_count) % sizeof(u->rxq)] = buf[i];
        u->rxq_count++;
    }
    uart_rx_fill(u);
    /*
     * With the BAM armed there is no FIFO interrupt to raise: the engine has
     * to move the bytes into the posted descriptor and retire it, which is
     * what wakes msm_serial.  Kick it right away (the idle timer would only
     * add latency, and input is already batched by the chardev read).
     */
    if (u->dmen & UART_DMEN_RX_BAM_ENABLE) {
        bam_uart_rx_kick();
    }
}

/* Report how much console output the guest produced, and value the SR. */
static uint32_t uart_sr(IPQUart *u)
{
    const char *forced = getenv("QEMU_IPQ_UART_SR");

    if (forced && *forced) {
        return env_u32("QEMU_IPQ_UART_SR", 0x0f);
    }
    return 0x0c |                    /* TX_READY | TX_EMPTY */
           (u->rx_count ? 0x01 : 0) |
           (u->rx_count >= UART_RX_FIFO_SIZE ? 0x02 : 0);
}

static uint64_t uart_read(void *opaque, hwaddr off, unsigned size)
{
    IPQUart *u = opaque;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: read  off 0x%03" HWADDR_PRIx "%s\n",
                      u->name, off,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    if (env_true("QEMU_IPQ_UART_RX_LOG") &&
        (off == 0x08 || off == 0x0c || off == 0x10 || off == 0x14 ||
         off == 0x34 || off == 0x38 || off == 0x3c ||
         off == 0x70 || off == 0x100) &&
        /*
         * Level 2 drops the idle polls.  A bootloader waiting for a key reads
         * 0x10 millions of times a second, and logging that is what once
         * filled /tmp; with "nothing buffered" reads filtered out the log is
         * only the interesting transitions.
         */
        (env_u32("QEMU_IPQ_UART_RX_LOG", 1) < 2 ||
         u->rx_count || u->rx_stale)) {
        UART_RX_LOG("%s read off=%02x size=%u rx_count=%u stale=%d%s\n",
                    u->name, (unsigned)off, size, u->rx_count,
                    u->rx_stale, trace_pc());
    }
    switch (off) {
    case 0x00:                  /* MR1 */
        return u->mr1;
    case 0x04:                  /* MR2 */
        return u->mr2;
    case 0x08:                  /* SR (UARTDM and legacy numbering) */
    case 0xa4:
        return uart_sr(u);
    case 0x10:                  /* MISR (masked interrupt status) */
        return uart_isr(u) & u->imr;
    case 0x14:                  /* ISR (unmasked) */
        return uart_isr(u);
    case 0x1c:                  /* TFWR */
        return u->tfwr;
    case 0x20:                  /* RFWR: whole words available */
        return u->rx_count / 4;
    case 0x34:                  /* DMRX */
        return u->dmrx;
    case 0x38: {                /* RX_TOTAL_SNAP */
        /*
         * The register means "how many bytes the receiver has taken in", and
         * what that counts depends on who is draining the port:
         *
         *  - PIO (DMEN RX BAM clear): the bytes are still in the FIFO, and
         *    that is exactly what the bootloaders want.  RouterBOOT's getc()
         *    (0x81001e84) reads this once, keeps it in its own counter,
         *    consumes at most four bytes of the FIFO word and decrements it
         *    per byte, so `min(snap,4)` must be the number of valid bytes in
         *    the word it is about to read -- returning 0 made every key press
         *    a zero-length character that was silently thrown away.
         *
         *  - BAM/DMA (DMEN RX BAM set): the bytes never touch the FIFO, they
         *    are written straight into the posted descriptor, so the count is
         *    the bytes moved to DRAM since the buffer was posted.
         *    msm_complete_rx_dma() takes `snap - old_snap_state` and then
         *    zeroes its own cached value, so the counter restarts here.
         */
        uint32_t snap = uart_bam_rx_armed() ? u->rx_total : u->rx_count;

        UART_RX_LOG("%s SNAP -> %u\n", u->name, snap);
        if (uart_bam_rx_armed()) {
            u->rx_total = 0;
        }
        return snap;
    }
    case 0x3c:                  /* DMEN */
        return u->dmen;
    case 0x40:                  /* NCF_TX */
        return u->ncf_tx;
    case 0x44:                  /* TCA: report the FIFO empty (SBL1) */
        return env_u32("QEMU_IPQ_UART_FIFO", 0x100);
    case 0x4c:                  /* TCA companion */
        return 0;
    case 0x50:                  /* RXFS: words available, low 3 bits */
        return (u->rx_count / 4) & 0x7;
    case 0x70:                  /* UARTDM_RF: RX FIFO */
    case 0x100: {
        uint8_t buf[4];

        /*
         * The FIFO hands out whole words, but the register also decodes
         * byte accesses (the early console and a few tty paths read it that
         * way).  A byte access must consume exactly one byte or the rest of
         * the word is lost.
         */
        if (size < 4) {
            uint8_t one = 0;

            uart_rx_pop_byte(u, &one);
            return one;
        }
        uart_rx_pop(u, buf);
        return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
               ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    }
    default:
        return env_u32("QEMU_IPQ_UART_SR", 0) ? 0 : 0;
    }
}

static void uart_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    IPQUart *u = opaque;

    if (env_true("QEMU_IPQ_TRACE")) {
        qemu_log_mask(LOG_UNIMP, "%s: write off 0x%03" HWADDR_PRIx
                      " size %u <- 0x%08" PRIx64 "%s\n", u->name, off, size,
                      val,
                      env_u32("QEMU_IPQ_TRACE", 0) > 1 ? trace_pc() : "");
    }
    /*
     * TF (UARTDM) lives at 0x70..0x7f and 0x100: it is a 4-byte-wide staging
     * register, and the msm_serial driver packs up to four characters into a
     * single 32-bit store (low byte first), padding a partial group with
     * NULs -- e.g. 0x2020205b is "[   ".  Emit every byte of the store and
     * drop the NUL padding, so the log is what the wire actually carries.
     * earlycon drives the same register one character at a time.
     */
    if ((off >= 0x70 && off < 0x80) || (off >= 0x100 && off < 0x110)) {
        uint8_t buf[4];
        unsigned n = 0, k;

        if (size > sizeof(buf)) {
            size = sizeof(buf);
        }
        for (k = 0; k < size; k++) {
            uint8_t c = (val >> (8 * k)) & 0xff;

            if (c) {
                buf[n++] = c;
            }
        }
        if (n) {
            for (k = 0; k < n; k++) {
                die_dbg_ser(buf[k]);
            }
            qemu_chr_fe_write_all(&u->chr, buf, n);
        }
        uart_update_irq(u);
        return;
    }
    if (env_true("QEMU_IPQ_UART_RX_LOG") &&
        (off == 0x10 || off == 0x14 || off == 0x34 || off == 0x3c)) {
        UART_RX_LOG("%s write off=%02x <- %08x%s\n", u->name,
                    (unsigned)off, (uint32_t)val, trace_pc());
    }
    switch (off) {
    case 0x00:
        u->mr1 = val;
        break;
    case 0x04:
        u->mr2 = val;
        break;
    case 0x08:
        /* Legacy SR (reset commands arrive through CR on UARTDM). */
        break;
    case 0x10:                  /* CR: reset/tx/rx enable commands */
        u->cr = val;
        if ((val >> 4 & 0xf) == 1) {        /* RESET_RX */
            if (u->dmen & UART_DMEN_RX_BAM_ENABLE) {
                bam_uart_rx_idle();
            }
            u->rx_head = u->rx_count = 0;
            u->rx_stale = false;
            timer_del(u->stale_timer);
        } else if ((val >> 4 & 0xf) == 3) { /* RESET_ERROR_STATUS */
            /*
             * RouterBOOT's getc() writes CR 0x30 and re-initialises the port
             * when it reads a stale receive word (SR bit 4), so the error has
             * to be cleared here or it would retry against the same state.
             */
            u->rx_stale = false;
            timer_del(u->stale_timer);
        } else if ((val >> 4 & 0xf) == 8) { /* RESET_STALE_INT */
            u->rx_stale = false;
            timer_del(u->stale_timer);
        }
        break;
    case 0x14:                  /* IMR */
        u->imr = val;
        break;
    case 0x1c:
        u->tfwr = val;
        break;
    case 0x20:
        u->rfwr = val;
        break;
    case 0x34:                  /* DMRX also resets the snapshot count */
        u->dmrx = val;
        u->rx_total = 0;
        break;
    case 0x3c:
        u->dmen = val;
        break;
    case 0x40:
        u->ncf_tx = val;
        break;
    default:
        break;
    }
    uart_update_irq(u);
}

static const MemoryRegionOps uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void add_uart(MemoryRegion *parent, hwaddr base, const char *name,
                     Chardev *chr, qemu_irq irq)
{
    IPQUart *u = g_new0(IPQUart, 1);
    Error *err = NULL;

    u->name = name;
    u->irq = irq;
    u->mr1 = 0;
    u->mr2 = 0;
    u->stale_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, uart_stale_expire, u);
    if (!qemu_chr_fe_init(&u->chr, chr, &err)) {
        error_report_err(err);
        exit(1);
    }
    qemu_chr_fe_set_handlers(&u->chr, uart_can_receive, uart_receive,
                             NULL, NULL, u, NULL, true);
    memory_region_init_io(&u->mr, NULL, &uart_ops, u, name, 0x1000);
    memory_region_add_subregion(parent, base, &u->mr);
    if (!ipq_console_uart) {
        ipq_console_uart = u;
    }
}

/* Bytes handed to the console UART by its BAM TX pipe (DMA console output). */
static void uart_bam_write(const uint8_t *buf, size_t n)
{
    size_t i;

    if (!ipq_console_uart) {
        return;
    }
    for (i = 0; i < n; i++) {
        die_dbg_ser(buf[i]);
    }
    qemu_chr_fe_write_all(&ipq_console_uart->chr, buf, n);
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
    fprintf(stderr, "ipq4019: loaded %s (SBL ELF), entry 0x%08x, %d phdrs\n",
           path, ipq4019_elf_entry, eh->e_phnum);
    g_free(buf);
    return true;
}

/*
 * Power-on reset of the modelled hardware.
 *
 * A system reset drops the SoC's state, and the boot chain depends on it: the
 * second SBL1 run re-probes the SPI controller and the BAM assuming they are
 * in their reset state, so leaving Linux's leftovers in the QUP's FIFOs makes
 * boot_flash_init fail with a die that is perfectly fine.  The one thing that
 * must survive is the flash array itself -- that is the point of a reboot.
 */
static void ipq4019_dev_reset(void *opaque)
{
    int i;

    if (env_true("QEMU_IPQ_NO_DEV_RESET")) {
        return;
    }

    /* Start the QTIMER/GCNT/XO/TSC frames from zero again (SBL1's log stamps,
     * udelay() baselines and the kernel's early delays all read them). */
    ipq_counter_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (qup_spi && !env_true("QEMU_IPQ_NO_RESET_QUP")) {
        IPQQupl *q = qup_spi;
        uint8_t *resp = q->resp;
        uint32_t resp_cap = q->resp_cap;
        qemu_irq irq = q->irq;

        memset((char *)q + offsetof(IPQQupl, phase), 0,
               sizeof(*q) - offsetof(IPQQupl, phase));
        q->resp = resp;
        q->resp_cap = resp_cap;
        q->irq = irq;
        memset(q->shadow, 0, sizeof(q->shadow));
    }

    if (ipq_blsp1_bam && !env_true("QEMU_IPQ_NO_RESET_BAM")) {
        IPQBam *b = ipq_blsp1_bam;

        memset(b->shadow, 0, b->nregs * sizeof(uint32_t));
        memset(b->valid, 0, b->nregs);
        for (i = 0; i < BAM_PIPE_COUNT; i++) {
            BAMClient client = b->pipe[i].client;

            memset(&b->pipe[i], 0, sizeof(b->pipe[i]));
            b->pipe[i].client = client;
        }
        bam_seed_id(b);
    }

    if (ipq_console_uart && !env_true("QEMU_IPQ_NO_RESET_UART")) {
        IPQUart *u = ipq_console_uart;

        timer_del(u->stale_timer);
        u->mr1 = u->mr2 = u->imr = u->cr = 0;
        u->tfwr = u->rfwr = u->dmrx = u->dmen = u->ncf_tx = 0;
        u->rx_head = u->rx_count = 0;
        u->rxq_head = u->rxq_count = 0;
        u->rx_total = 0;
        u->rx_stale = false;
        uart_update_irq(u);
    }

    if (ipq_mdio && !env_true("QEMU_IPQ_NO_RESET_MDIO")) {
        ipq_mdio->mode = ipq_mdio->addr = ipq_mdio->wdata = 0;
        ipq_mdio->rdata = ipq_mdio->cmd = 0;
        for (i = 0; i < ipq_mdio->nphy; i++) {
            phy_reset_regs(&ipq_mdio->phy[i]);
        }
    }

    if (ipq_edma && !env_true("QEMU_IPQ_NO_RESET_EDMA")) {
        memset(ipq_edma->regs, 0,
               ipq_edma->nregs * sizeof(uint32_t));
        memset(ipq_edma->tx_pkt, 0, sizeof(ipq_edma->tx_pkt));
        memset(ipq_edma->tx_byte, 0, sizeof(ipq_edma->tx_byte));
        memset(ipq_edma->tx_level, 0, sizeof(ipq_edma->tx_level));
    }

    if (!env_true("QEMU_IPQ_NO_RESET_STUBS")) {
        for (i = 0; ipq_stub_list && i < (int)ipq_stub_list->len; i++) {
            IPQStub *st = g_ptr_array_index(ipq_stub_list, i);

            if (st->nregs) {
                memset(st->shadow, 0, st->nregs * sizeof(uint32_t));
                memset(st->valid, 0, st->nregs);
            }
            st->spin = 0;
        }
    }

    if (ipq_pshold) {
        ipq_pshold->val = 1;            /* rails back up */
    }

    if (ipq_wdt) {
        timer_del(ipq_wdt->bite);
        memset(ipq_wdt->regs, 0, sizeof(ipq_wdt->regs));
    }
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
static QEMUTimer *ipq_timer_dbg;

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
    timer_mod_ns(ipq_timer_dbg,
                 qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
}

/*
 * Debug: host-clock heartbeat (QEMU_IPQ_DIAG=1).  Runs on QEMU_CLOCK_HOST so
 * that it keeps printing even if the virtual clock has stopped, which is the
 * only way to tell "guest spins" from "guest waits for a timer forever".
 */
static QEMUTimer *ipq_diag_timer;
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
    if (ipq_edma) {
        fprintf(stderr, "diag: net tx=%" PRIu64 " (noport=%" PRIu64 " nolink=%" PRIu64
               ") rx=%" PRIu64 " (dropped=%" PRIu64 ")\n",
               ipq_edma->tx_host, ipq_edma->tx_drop_noport,
               ipq_edma->tx_drop_nolink, ipq_edma->rx_host,
               ipq_edma->rx_dropped);
    }
    fflush(stdout);
    ipq_diag_last_virtual = v;
    timer_mod_ns(ipq_diag_timer, h + 2 * NANOSECONDS_PER_SECOND);
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

    if (env_true("QEMU_IPQ_DIAG")) {
        ipq_diag_last_virtual = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ipq_diag_timer = timer_new_ns(QEMU_CLOCK_HOST, ipq4019_diag, NULL);
        timer_mod_ns(ipq_diag_timer,
                     qemu_clock_get_ns(QEMU_CLOCK_HOST) +
                     2 * NANOSECONDS_PER_SECOND);
    }

    if (env_true("QEMU_IPQ_TIMER_DBG")) {
        ipq_timer_dbg = timer_new_ns(QEMU_CLOCK_VIRTUAL, ipq4019_timer_dbg,
                                     NULL);
        timer_mod_ns(ipq_timer_dbg, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
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
    ipq_dram_mr = machine->ram;
    ipq_dram_size = machine->ram_size;
    qemu_register_reset(ipq_reset_dump, NULL);

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
        fprintf(stderr, "ipq4019: flash die %u MiB, RDID", (unsigned)(die / MiB));
        for (j = 0; j < ipq_flash_jedec_len; j++) {
            fprintf(stderr, " %02x", ipq_flash_jedec[j]);
        }
        fprintf(stderr, ", window %u MiB <- %s (%zu bytes)\n",
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
    {
        IPQPsHold *ps = g_new0(IPQPsHold, 1);
        MemoryRegion *mr = g_new0(MemoryRegion, 1);

        ps->val = 1;                    /* rails held up out of reset */
        ipq_pshold = ps;
        memory_region_init_io(mr, NULL, &pshold_ops, ps, "pshold", 0x1000);
        memory_region_add_subregion(sysmem, IPQ_PSHOLD_BASE, mr);
    }
    {
        IPQWdt *w = g_new0(IPQWdt, 1);

        w->hz = env_u32("QEMU_IPQ_WDT_HZ", 32768);
        w->bite = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_bite, w);
        ipq_wdt = w;
        memory_region_init_io(&w->mr, NULL, &wdt_ops, w, "wdt", 0x1000);
        /* Above the apcs stub that covers 0x0b000000..0x0b01ffff. */
        memory_region_add_subregion_overlap(sysmem, IPQ_WDT_BASE, &w->mr, 1);
    }
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
    ipq_tlmm_cs_pin = env_u32("QEMU_IPQ_CS_GPIO", 54);
    add_tlmm(sysmem, IPQ_TLMM_BASE, IPQ_TLMM_SIZE, "tlmm");
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
    ipq_blsp1_bam = bam;
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
            if (qup_spi) {
                qup_spi->irq = qdev_get_gpio_in(gicdev, 95);
            }
            ipq_gic_device = gicdev;

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
         * ever appear.
         */
        ess->regs[0] = 0x1402;
        /* command registers whose busy bit clears when the op completes */
        regfile_clr_bit(ess, 0x034, 1u << 17);      /* QCA8K_MIB_BUSY */
        regfile_clr_bit(ess, 0x60c, 1u << 31);      /* QCA8K_ATU_FUNC_BUSY */
        regfile_clr_bit(ess, 0x614, 1u << 31);      /* QCA8K_VTU_FUNC1_BUSY */
    }
    add_edma(sysmem, 0x0c080000, 0x00080000);
    if (!getenv("QEMU_IPQ_EDMA_NO_IRQ")) {
        edma_wire_irqs(ipq_gic_device);
    }
    /* Attach the engine to a host backend, if the user gave us one. */
    edma_connect_netdev(ipq_edma);
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

    /*
     * Console UARTs.  BLSP1 UART1 (0x78af000) is <GIC_SPI 107> in the DTS and
     * BLSP1 UART2 (0x78b0000) is <GIC_SPI 108>; arm_gic's input line n is
     * SPI n, same mapping as the BAM and QUP above.
     */
    add_uart(sysmem, IPQ_UART0_BASE, "uart0", serial_hd(0),
             ipq_gic_device ? qdev_get_gpio_in(ipq_gic_device, 107) : NULL);
    if (serial_hd(1)) {
        add_uart(sysmem, IPQ_UART1_BASE, "uart1", serial_hd(1),
                 ipq_gic_device ? qdev_get_gpio_in(ipq_gic_device, 108) : NULL);
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
        fprintf(stderr, "ipq4019: boot-info handed to SBL1 in r0 = 0x%08x\n",
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
    qemu_register_reset(ipq4019_dev_reset, NULL);
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
