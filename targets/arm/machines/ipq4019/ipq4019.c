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
#include "ipq4019/qpic.h"

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
            ipq_qpic_set_irq(qdev_get_gpio_in(gicdev, 101));
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
