/*
 * MediaTek MT7621 (MIPS 1004Kc) bring-up machine
 *
 * This machine models the SoC, not the firmware: DDR at 0, the palmbus
 * peripherals at 0x1e000000, and a boot path that either
 *
 *   - takes a raw image (load-addr/entry properties), or
 *   - takes a uImage (the format the MT7621 BootROM loads from NAND),
 *     strips the 64-byte header and loads the payload at the header's
 *     load address.
 *
 * The first target is the Breed bootloader taken from a parallel-NAND
 * dump, followed by OpenWrt.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/char/serial.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/mips/mips.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "system/reset.h"
#include "target/mips/cpu-qom.h"
#include "target/mips/internal.h"

#define TYPE_MT7621_MACHINE MACHINE_TYPE_NAME("mt7621")
OBJECT_DECLARE_SIMPLE_TYPE(MT7621State, MT7621_MACHINE)

/* SoC memory map */
#define MT7621_RAM_BASE         0x00000000
#define MT7621_PERIPH_BASE      0x1e000000
#define MT7621_PERIPH_SIZE      0x00200000
#define MT7621_SYSCTL_OFF       0x00000000
#define MT7621_UART0_OFF        0x00000c00
#define MT7621_NFI_OFF          0x00003000
#define MT7621_DRAMC_OFF        0x00005000
#define MT7621_SRAM_BASE        0x1e100000
#define MT7621_SRAM_SIZE        0x00010000

#define MT7621_UART_CLOCK       50000000
#define MT7621_UART_BAUD_BASE   (MT7621_UART_CLOCK / 16)

/* sysctl register offsets we care about */
#define SYS_CHIP_REV_ID         0x0c
#define SYS_SYSCFG0             0x10
#define SYS_CLKCFG0             0x2c
#define SYS_RSTCTL              0x34
#define SYS_CUR_CLK_STS         0x44
#define SYS_GPIO_MODE           0x60

#define SYSCTL_WORDS            (0x1000 / 4)
#define DRAMC_WORDS             (0x1000 / 4)
#define NFI_WORDS               (0x1000 / 4)

struct MT7621State {
    MachineState parent;

    MIPSCPU *cpu;
    Clock *cpuclk;

    MemoryRegion ram;
    MemoryRegion periph;
    MemoryRegion sysctl;
    MemoryRegion dramc;
    MemoryRegion nfi;
    MemoryRegion sram;

    uint32_t sysctl_regs[SYSCTL_WORDS];
    uint32_t dramc_regs[DRAMC_WORDS];
    uint32_t nfi_regs[NFI_WORDS];

    /* boot image */
    uint32_t load_addr;
    uint32_t entry;
    bool uimage;
};

static uint64_t mt7621_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;

    return regs[(addr & 0xfff) >> 2];
}

static void mt7621_reg_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    uint32_t *regs = opaque;

    regs[(addr & 0xfff) >> 2] = val;
}

static const MemoryRegionOps mt7621_reg_ops = {
    .read = mt7621_reg_read,
    .write = mt7621_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt7621_irq_noop(void *opaque, int n, int level)
{
    /* No interrupt controller yet: the console is polled at this stage. */
}

static void mt7621_load_image(MT7621State *s, const char *filename)
{
    uint8_t hdr[64];
    uint8_t *buf;
    uint64_t load, entry, size;
    off_t off = 0, total;
    int fd;

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        error_report("mt7621: cannot open %s: %s", filename, strerror(errno));
        exit(1);
    }

    total = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (pread(fd, hdr, sizeof(hdr), 0) != sizeof(hdr)) {
        error_report("mt7621: %s: short read", filename);
        exit(1);
    }

    if (s->uimage || ldl_be_p(hdr) == 0x27051956) {
        if (ldl_be_p(hdr) != 0x27051956) {
            error_report("mt7621: %s: not a uImage", filename);
            exit(1);
        }
        size  = (uint32_t)ldl_be_p(hdr + 12);
        load  = (uint32_t)ldl_be_p(hdr + 16);
        entry = (uint32_t)ldl_be_p(hdr + 20);
        off   = 64;
        if (s->load_addr) {
            load = s->load_addr;
        }
        if (s->entry) {
            entry = s->entry;
        }
        if ((load & 0x1fffffff) + size > s->ram.size) {
            error_report("mt7621: image 0x%" PRIx64 "+0x%" PRIx64
                         " does not fit in RAM", load, size);
            exit(1);
        }
        printf("mt7621: uImage payload 0x%" PRIx64 " bytes -> 0x%" PRIx64
               ", entry 0x%" PRIx64 "\n", size, load, entry);
    } else {
        load  = s->load_addr;
        entry = s->entry;
        size  = total;
        printf("mt7621: raw image 0x%" PRIx64 " bytes -> 0x%" PRIx64
               ", entry 0x%" PRIx64 "\n", size, load, entry);
    }

    if (size > total - off) {
        error_report("mt7621: %s: truncated image", filename);
        exit(1);
    }

    buf = g_malloc(MIN(size, 1 * MiB));
    while (size) {
        size_t chunk = MIN(size, 1 * MiB);
        ssize_t n = pread(fd, buf, chunk, off);

        if (n != chunk) {
            error_report("mt7621: %s: short read at 0x%" PRIx64,
                         filename, (uint64_t)off);
            exit(1);
        }
        {
            hwaddr paddr = load;

            /* KSEG0/KSEG1: the image header uses the uncached alias. */
            if (paddr >= 0x80000000 && paddr < 0xc0000000) {
                paddr &= 0x1fffffff;
            }
            address_space_write(&address_space_memory, paddr,
                                MEMTXATTRS_UNSPECIFIED, buf, chunk);
        }
        load += chunk;
        off  += chunk;
        size -= chunk;
    }
    g_free(buf);
    close(fd);

    s->entry = entry;
}

static void mt7621_reset(void *opaque)
{
    MT7621State *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    /* Malta does the same when a -kernel image is supplied. */
    env->CP0_Status &= ~(1 << 2); /* CP0St_ERL */
    env->active_tc.PC = s->entry;
}

static void mt7621_machine_init(MachineState *machine)
{
    MT7621State *s = MT7621_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    qemu_irq uart_irq;
    uint64_t ram_size = machine->ram_size;

    if (ram_size > 512 * MiB) {
        error_report("mt7621: at most 512 MiB of RAM is supported");
        exit(1);
    }

    /* CPU */
    s->cpuclk = clock_new(OBJECT(s), "cpu-refclk");
    clock_set_hz(s->cpuclk, 880 * 1000 * 1000);

    s->cpu = mips_cpu_create_with_clock(machine->cpu_type, s->cpuclk, false);
    cpu_mips_irq_init_cpu(s->cpu);
    cpu_mips_clock_init(s->cpu);
    qemu_register_reset(mt7621_reset, s);

    /* DDR */
    memory_region_init_ram(&s->ram, NULL, "mt7621.ddr", ram_size,
                           &error_fatal);
    memory_region_add_subregion(sysmem, MT7621_RAM_BASE, &s->ram);

    /* palmbus */
    memory_region_init(&s->periph, NULL, "mt7621.periph",
                       MT7621_PERIPH_SIZE);

    memset(s->sysctl_regs, 0, sizeof(s->sysctl_regs));
    s->sysctl_regs[SYS_CHIP_REV_ID / 4]  = 0x00000000;
    s->sysctl_regs[SYS_SYSCFG0 / 4]      = 0x00000180; /* 25 MHz xtal, DDR3 */
    s->sysctl_regs[SYS_CLKCFG0 / 4]      = 0x00000000; /* CPU PLL 500 MHz */
    s->sysctl_regs[SYS_CUR_CLK_STS / 4]  = 0x00000101; /* /1 * 1 */
    memory_region_init_io(&s->sysctl, NULL, &mt7621_reg_ops, s->sysctl_regs,
                          "mt7621.sysctl", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_SYSCTL_OFF, &s->sysctl);

    memset(s->dramc_regs, 0, sizeof(s->dramc_regs));
    s->dramc_regs[0x604 / 4] = 0x00000000;
    s->dramc_regs[0x618 / 4] = 0x00000190;
    s->dramc_regs[0x648 / 4] = 0x03800000;
    memory_region_init_io(&s->dramc, NULL, &mt7621_reg_ops, s->dramc_regs,
                          "mt7621.dramc", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_DRAMC_OFF, &s->dramc);

    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    memory_region_init_io(&s->nfi, NULL, &mt7621_reg_ops, s->nfi_regs,
                          "mt7621.nfi", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_NFI_OFF, &s->nfi);

    /* Boot SRAM: the BootROM/stage1 handshake lives at 0x1e10ad00. */
    memory_region_init_ram(&s->sram, NULL, "mt7621.sram",
                           MT7621_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, MT7621_SRAM_BASE, &s->sram);

    uart_irq = qemu_allocate_irq(mt7621_irq_noop, s, 0);
    serial_mm_init(&s->periph, MT7621_UART0_OFF, 2, uart_irq,
                   MT7621_UART_BAUD_BASE, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    memory_region_add_subregion(sysmem, MT7621_PERIPH_BASE, &s->periph);

    {
        const char *e = getenv("MT7621_ENTRY");
        const char *l = getenv("MT7621_LOAD");

        if (e) {
            s->entry = strtoul(e, NULL, 0);
        }
        if (l) {
            s->load_addr = strtoul(l, NULL, 0);
        }
    }

    /* Boot image */
    if (machine->kernel_filename) {
        mt7621_load_image(s, machine->kernel_filename);
    } else {
        error_report("mt7621: no boot image (-kernel) supplied");
        exit(1);
    }
}


static void mt7621_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "MediaTek MT7621 (MIPS 1004Kc)";
    mc->init = mt7621_machine_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("34Kf");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mt7621.ram";
    mc->max_cpus = 1;
}

static const TypeInfo mt7621_machine_type = {
    .name = TYPE_MT7621_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MT7621State),
    .class_init = mt7621_machine_class_init,
};

static void mt7621_machine_register_types(void)
{
    type_register_static(&mt7621_machine_type);
}

type_init(mt7621_machine_register_types);
