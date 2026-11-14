/*
 * Qualcomm IPQ4018/IPQ4019 -- the machine's generic blocks.
 *
 * Not every register block needs a model: most read back what was written,
 * which is enough for a driver's read-modify-write probe.  These are the
 * shadowed stub and its register-file sibling, the power-hold register (the
 * "power off" the guest can reach), the watchdog, and the traced memory
 * window used for the boot flash and the PBL context.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "target/arm/cpu-qom.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "ipq4019.h"

/* ------------------------------------------------------------------ */
/* generic shadowed stub                                               */
/* ------------------------------------------------------------------ */



void stub_pin(IPQStub *s, uint32_t off, uint32_t val)
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


const char *trace_pc(void)
{
    static char buf[48];
    CPUState *cs = current_cpu;

    if (!cs) {
        return "";
    }
    snprintf(buf, sizeof(buf), " pc=%08x lr=%08x",
             (uint32_t)ARM_CPU(cs)->env.regs[15],
             (uint32_t)ARM_CPU(cs)->env.regs[14]);
    return buf;
}

/* Host milliseconds since QEMU started, for "when did that access happen". */
const char *trace_ms(void)
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

/* QEMU_IPQ_STATE_DUMP=<path>: dump the whole model state at the handoff. */

/*
 * Modelled register files that keep software's writes (the ESS block, for
 * now).  They are not IPQStub, so the stub sweep does not reach them, but a
 * system reset has to clear them for the same reason: the next boot prints
 * "Switch id detected ..." out of an id field the previous OS already
 * rewrote.
 */

IPQStub *add_stub_ex(MemoryRegion *parent, hwaddr base, hwaddr size,
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
    if (!ipq->stub_list) {
        ipq->stub_list = g_ptr_array_new();
    }
    g_ptr_array_add(ipq->stub_list, s);
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
 * Powering the board off.
 *
 * This SoC has no register that cuts power.  The board's rails are held by
 * PS_HOLD, and software that wants to *restart* drops it (that is what
 * /sbin/reboot does; the pshold model above turns it into a machine reset).
 * A guest that wants to *stop* has nowhere to write: MikroTik's device tree
 * gives the kernel only `qcom,pshold`, which is a restart handler, so
 * /system shutdown prints
 *
 *     reboot: Power down
 *
 * from kernel_power_off() and then arch/arm's default machine_power_off()
 * falls into machine_halt(): mask IRQs, `while (1) cpu_do_idle();`.  On the
 * real board that is also the end of the story -- the part sits there until
 * something removes power.
 *
 * What that loop looks like from the outside is not "a halted CPU".  WFI is
 * only allowed to halt when nothing is pending (arm_cpu_has_work()), and the
 * interrupted line is still asserted because the CPU masked it, so WFI
 * returns at once and the loop spins: PC pinned on the WFI, IRQs masked,
 * CPU_INTERRUPT_HARD permanently set -- a full core burnt, which is exactly
 * the 100% CPU a hung shutdown shows.  A live guest never sits in that state:
 * idle halts with nothing pending, and any interrupt it masks while doing
 * work is taken as soon as it unmasks.
 *
 * So that state, held for IPQ_POWEROFF_TICKS, *is* the power-off: the machine
 * ends the way the plug coming out would.  poweroff=off keeps it running.
 */
#define IPQ_POWEROFF_MS     100
#define IPQ_POWEROFF_TICKS  15          /* 1.5 s of an IRQ-masked WFI spin */

static QEMUTimer *ipq_poweroff_timer;
static int ipq_poweroff_ticks;

static void ipq_poweroff_poll(void *opaque)
{
    CPUState *cs = qemu_get_cpu(0);
    ARMCPU *cpu;
    bool parked;

    if (!cs) {
        return;
    }
    cpu = ARM_CPU(cs);
    /*
     * IRQs masked, an interrupt that cannot be delivered, and a CPU that is
     * not halted: WFI returns immediately and machine_halt() spins on it.
     */
    parked = (cpu->env.daif & PSTATE_I) != 0 &&
             !cs->halted &&
             (cs->interrupt_request & CPU_INTERRUPT_HARD) != 0;

    if (env_true("QEMU_IPQ_HANG_LOG")) {
        fprintf(stderr, "ipq4019: poll pc=%08x daif=%02x halted=%d irq=%x "
                "ticks=%d\n", (uint32_t)cpu->env.regs[15],
                (uint32_t)cpu->env.daif,
                cs->halted, cs->interrupt_request, ipq_poweroff_ticks);
    }

    /* A paused machine (-S, a monitor stop) is not a powered-down one. */
    if (!runstate_is_running()) {
        ipq_poweroff_ticks = 0;
    } else if (ipq_cfg.poweroff && parked) {
        if (++ipq_poweroff_ticks >= IPQ_POWEROFF_TICKS) {
            fprintf(stderr,
                    "ipq4019: poweroff: the guest parked the CPU at 0x%08x "
                    "with IRQs masked and an interrupt pending, so the board "
                    "is off (poweroff=off in -machine keeps it running)\n",
                    (uint32_t)cpu->env.regs[15]);
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
            return;
        }
    } else {
        ipq_poweroff_ticks = 0;
    }
    timer_mod(ipq_poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IPQ_POWEROFF_MS);
}

void ipq_poweroff_start(void)
{
    ipq_poweroff_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ipq_poweroff_poll,
                                      NULL);
    timer_mod(ipq_poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + IPQ_POWEROFF_MS);
}

/*
 * KPSS watchdog at 0x0b017000: how RouterBOOT restarts the board.
 *
 * Leaving the setup menu calls this and then spins in `b .` waiting for the
 * chip to go away (0x81001b28):
 *
 *   mov r0, #0x100        ; bite time, 32.768 kHz ticks
 *   bl  0x81001acc
 *   b   .
 *
 * and 0x81001acc is mainline qcom_wdt_restart(), write for write:
 *
 *   [0x08] = 0            ; WDT_EN   = 0   disable
 *   [0x04] = 1            ; WDT_RST  = 1   ping
 *   [0x14] = r0           ; BITE_TIME      hard reset after r0 ticks
 *   [0x10] = -1           ; BARK_TIME      pretimeout never
 *   [0x08] = 1            ; WDT_EN   = 1   enable
 *
 * The register layout is the one drivers/watchdog/qcom-wdt.c uses for
 * "qcom,kpss-wdt" (reg_offset_data_kpss), NOT the older apcs-timer layout:
 *
 *   0x04 RST   0x08 EN   0x0c STS   0x10 BARK_TIME   0x14 BITE_TIME
 *
 * (an earlier version of this model had RST and EN swapped, which turned the
 * kernel's "disable" into a re-arm -- see below).
 *
 * The same routine, with r0 = 0x10000 (2 s), is also how an ordinary boot
 * hands over to the kernel:
 *
 *   0x810095d0  mov r0, #0x10000
 *   0x810095d4  bl  0x81001acc
 *
 * so the model must not fire that one late: Linux's qcom-wdt probes the block,
 * sees it running and stops it (measured: [0x04]<-1, [0x08]<-0 from
 * 0x80127dc4, inside the kernel).  With the correct layout that write clears
 * EN and disarms the countdown, exactly as the driver intends.  With RST/EN
 * swapped it looked like "enable + kick", the 2 s bite then fired during a
 * healthy boot, and the board bootlooped.
 *
 * BARK is the pretimeout (an interrupt, SPI 4 on this SoC); nothing in these
 * images sets it to anything but "never", and the machine does not wire the
 * bark line to the GIC, so bark only records status.  BITE is the reset.
 *
 * QEMU_IPQ_WDT_LOG=1 traces every access; QEMU_IPQ_NO_WDT_RESET=1 keeps the
 * register file without letting the bite reset the machine.
 */
#define WDT_RST        0x04
#define WDT_EN         0x08
#define WDT_STATUS     0x0c
#define WDT_BARK_TIME  0x10
#define WDT_BITE_TIME  0x14




static uint64_t wdt_read(void *opaque, hwaddr off, unsigned size)
{
    IPQWdt *w = opaque;

    return w->regs[(off & 0xfff) / 4];
}

void wdt_disarm(IPQWdt *w)
{
    timer_del(w->bark);
    timer_del(w->bite);
}

/*
 * (Re)start both countdowns from now.
 *
 * Every write while the block is enabled counts as a kick: the bootloader
 * programs the time registers after the disable and finishes with the enable,
 * while a driver's ping is a write to WDT_RST.  Both end up re-arming here.
 *
 * 0xffffffff means "never" (RouterBOOT uses it for BARK) and 0 means the
 * timeout has not been programmed yet, which must not count as "bite now":
 * the enable arrives before the time registers, and OpenWrt's U-Boot enables
 * the block while programming a different, vendor-specific register set
 * ([0x00]=1 ping, [0x24]=0xea600, [0x08]=1 enable, no BITE_TIME at all), so a
 * zero timeout there would reset the board during U-Boot's own network
 * initialisation -- which is exactly the bootloop it caused.
 */
static void wdt_arm(IPQWdt *w)
{
    uint32_t hz = env_u32("QEMU_IPQ_WDT_HZ", 32768);
    uint32_t bark, bite;
    int64_t now;

    wdt_disarm(w);
    if (!(w->regs[WDT_EN / 4] & 1)) {
        return;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bark = w->regs[WDT_BARK_TIME / 4];
    bite = w->regs[WDT_BITE_TIME / 4];
    if (bark != 0xffffffffu && bark) {
        timer_mod(w->bark, now + (int64_t)bark * NANOSECONDS_PER_SECOND / hz);
    }
    if (bite != 0xffffffffu && bite) {
        timer_mod(w->bite, now + (int64_t)bite * NANOSECONDS_PER_SECOND / hz);
    }
}

static void wdt_bark_expire(void *opaque)
{
    IPQWdt *w = opaque;

    /* Pretimeout only: the bark line is not wired to the GIC in this machine. */
    w->regs[WDT_STATUS / 4] |= 1;
    if (env_true("QEMU_IPQ_WDT_LOG")) {
        fprintf(stderr, "wdt: bark (pretimeout), not resetting\n");
    }
}

static void wdt_bite_expire(void *opaque)
{
    IPQWdt *w = opaque;

    w->regs[WDT_STATUS / 4] |= 1;
    if (env_true("QEMU_IPQ_WDT_LOG")) {
        fprintf(stderr, "wdt: bite, %s\n",
                env_true("QEMU_IPQ_NO_WDT_RESET") ? "not resetting"
                                                  : "resetting");
    }
    if (!env_true("QEMU_IPQ_NO_WDT_RESET")) {
        wdt_disarm(w);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static void wdt_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    IPQWdt *w = opaque;
    hwaddr reg = off & 0xfff;

    if (env_true("QEMU_IPQ_WDT_LOG")) {
        fprintf(stderr, "wdt: [%02" HWADDR_PRIx "] <- 0x%08" PRIx64 "%s\n",
                reg, v, trace_pc());
    }
    w->regs[reg / 4] = v;

    if (reg == WDT_EN && !(v & 1)) {
        wdt_disarm(w);
        return;
    }
    wdt_arm(w);
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
uint64_t ipq_dram_size;

void ipq_reset_dump(void *opaque)
{
    const char *path = getenv("QEMU_IPQ_RESET_DUMP");
    const void *ram;
    FILE *f;

    if (!path || !*path || !ipq->dram) {
        return;
    }
    ram = memory_region_get_ram_ptr(ipq->dram);
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

void add_stub(MemoryRegion *parent, hwaddr base, hwaddr size,
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


/*
 * Clock rates handed to the gcnt/xo read handler as its opaque pointer.  These
 * must outlive ipq4019_init(), so they are file-scope rather than locals (a
 * dangling pointer here silently rescales every timer read).
 */
uint32_t gcnt_hz;
uint32_t qtimer_hz;
uint32_t xo_hz;
uint32_t tsc_hz;
/*
 * Every counter here reads "virtual clock * rate", so it counts from the
 * moment QEMU started.  A machine reset re-bases them, because the boot chain
 * stamps its own log from one of these: without it, SBL1's first line after a
 * guest reboot reads "13688788 - SBL1, Start" -- the seconds the machine had
 * already been up -- instead of something near zero.
 */
uint64_t ipq_counter_epoch_ns;

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

IPQMem *add_mem(MemoryRegion *parent, hwaddr base, uint64_t size,
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


/* The two blocks the machine instantiates itself, so their ops stay here. */
IPQPsHold *add_pshold(MemoryRegion *sysmem)
{
    IPQPsHold *ps = g_new0(IPQPsHold, 1);
    MemoryRegion *mr = g_new0(MemoryRegion, 1);

    ps->val = 1;                    /* rails held up out of reset */
    ipq->pshold = ps;
    memory_region_init_io(mr, NULL, &pshold_ops, ps, "pshold", 0x1000);
    memory_region_add_subregion(sysmem, IPQ_PSHOLD_BASE, mr);
    return ps;
}

IPQWdt *add_wdt(MemoryRegion *sysmem)
{
    IPQWdt *w = g_new0(IPQWdt, 1);

    w->bark = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_bark_expire, w);
    w->bite = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_bite_expire, w);
    ipq->wdt = w;
    memory_region_init_io(&w->mr, NULL, &wdt_ops, w, "wdt", 0x1000);
    /* The page sits inside the 0x0b000000 apcs stub. */
    memory_region_add_subregion_overlap(sysmem, IPQ_WDT_BASE, &w->mr, 1);
    return w;
}
