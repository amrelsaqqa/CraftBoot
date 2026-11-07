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

#define NAND_PAGE       2048u
/*
 * 2048+64, not 2048+128: the part on this board is a MX35LF1GE4AB, whose own
 * read-id answer was measured as `id=C212C212` -- manufacturer c2, device 12,
 * then the same two bytes again because the die repeats its id while it keeps
 * being clocked.  64 is also what the spare-area tags and the 128 MiB capacity
 * in that survey imply.
 */
#define NAND_OOB        64u
#define NAND_STRIDE     (NAND_PAGE + NAND_OOB)   /* one page incl. spare */
#define NAND_PPBLK      64u                      /* pages per erase block */
#define NAND_BLKBYTES   (NAND_STRIDE * NAND_PPBLK)   /* 128 KiB + spare */

#define NAND_BLOCKS_DEF 1024u                    /* 128 MiB of pages */
#define NAND_BYTES_DEF  (NAND_BLKBYTES * NAND_BLOCKS_DEF)   /* 1 Gbit + spare */


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

/*
 * QPIC tracing.  Gated by QEMU_IPQ_QPIC_LOG alone: the messages used to go
 * through qemu_log_mask(LOG_UNIMP), which needs `-d unimp` as well, so a run
 * started from lan1-bridge.sh (which passes no -d) logged nothing even with
 * the variable set.  Diagnostics that need a second, easy-to-forget flag are
 * diagnostics that are not there when they are needed.
 */
static void qpic_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void qpic_log(const char *fmt, ...)
{
    va_list ap;

    if (!env_true("QEMU_IPQ_QPIC_LOG")) {
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
typedef struct IPQStub IPQStub;
typedef struct IPQPsHold IPQPsHold;
typedef struct IPQWdt IPQWdt;
typedef struct IPQMem IPQMem;
typedef struct IPQQupl IPQQupl;
typedef struct IPQNand IPQNand;
typedef struct IPQTLMM IPQTLMM;
typedef struct IPQBam IPQBam;
typedef struct IPQRegFile IPQRegFile;
typedef struct IPQEdma IPQEdma;
typedef struct IPQMdio IPQMdio;
typedef struct IPQUart IPQUart;
typedef struct IPQBootStage IPQBootStage;
typedef struct IPQQpic IPQQpic;

typedef struct IPQ4019State {
    /* on-die buses and controllers */
    IPQQupl *qup_spi;           /* the one QUP the BAM pipes are wired to */
    IPQMdio *mdio;              /* MDIO master; the PHYs on it are board */
    IPQEdma *edma;
    IPQTLMM *tlmm;
    IPQBam *blsp1_bam;
    IPQUart *console_uart;
    DeviceState *gic;
    MemoryRegion *dram;
    GArray *mapped;             /* window table, for the state dump */
    GPtrArray *stub_list;       /* register-file stubs, for the state dump */
    GPtrArray *regfile_list;
    IPQPsHold *pshold;
    IPQWdt *wdt;

    /* the boot chain, read out of the configured boot device */
    const IPQBootStage *qsee_stage;
    const IPQBootStage *appsbl_stage;

    QEMUTimer *timer_dbg;
    QEMUTimer *diag_timer;
} IPQ4019State;

/*
 * The board's own visible parts.  An LED is an output pin with a lamp on it;
 * a key is an input pin with a switch on it.  Both are wiring, so both are
 * configuration -- the structs here are the parsed form of that.
 */
typedef struct IPQLed {
    char *name;
    uint32_t gpio;
    bool active_low;
    int level;                  /* the line level the guest is driving */
    bool on;                    /* what the lamp shows */
} IPQLed;

typedef struct IPQKey {
    char *name;
    uint32_t gpio;
    bool active_low;
    int qcode;                  /* QKeyCode, from the qapi enum */
    bool down;
} IPQKey;

typedef struct IPQBoardState {
    IPQMem *nor;                /* the die behind the QUP's chip select 0 */
    IPQNand *nand;              /* ... and chip select 1 */
    IPQQpic *qpic;              /* the die behind the QPIC parallel controller */

    IPQLed *leds;               /* the board's lamps, in configuration order */
    unsigned n_leds;
    IPQKey *keys;
    unsigned n_keys;

    DeviceState *led_panel;     /* the display window that shows them */
    DeviceState *key_device;    /* the input handler that presses them */
} IPQBoardState;

static IPQ4019State *ipq;       /* the SoC this machine is */
static IPQBoardState *ipq_board;/* the parts wired to it */

/* ------------------------------------------------------------------ */
/* the board: what is soldered around the SoC                          */
/* ------------------------------------------------------------------ */

/*
 * The SoC model above is fixed: the same die, the same controllers, the same
 * addresses.  Everything outside it is a board question -- which dies hang off
 * the QUP, how big they are, what they identify as, which one the boot chain
 * comes from -- and a board question belongs in configuration, not in code.
 *
 *   -machine ipq4019,board=default          today's wiring (a preset)
 *   -machine ipq4019,board=none             no die at all: the pure SoC
 *   -machine ipq4019,nor=on,nand=on,boot=spi-nand,...
 *
 * A preset is a compiled-in data table, nothing more: it fills the same
 * IPQBoardConfig that the explicit properties fill, and an explicit property
 * always wins over the preset, whatever order they were given in.  That is
 * why the setters only record what was given (ipq_cfg_set): the merge happens
 * once, in ipq_board_config_resolve(), when the machine starts.
 *
 * The environment knobs (QEMU_IPQ_FLASH_*, QEMU_IPQ_NAND_*) keep working as a
 * deprecated shim over the same struct, so every command line written before
 * this existed still selects the same board.
 */

/* the mask-ROM flash types SBL1 is told about (boot_pbl_v1.c switches on it) */
#define IPQ_FLASH_TYPE_NONE   0
#define IPQ_FLASH_TYPE_NAND   2
#define IPQ_FLASH_TYPE_SPI    6

/*
 * The reference design's switch: a QCA8075 exposes its five copper PHYs at
 * MDIO addresses 0..4 and its PSGMII control block at 5 (U-Boot's PSGMII_ID).
 * A board with a different switch, or fewer ports, names its own list.
 */
#define IPQ_ETH_DEFAULT_PORTS  "0,1,2,3,4,5"
#define IPQ_ETH_DEFAULT_PHY_ID 0x004dd0b1u      /* QCA8075 */
/*
 * The PSGMII control block is part of the switch, not a copper PHY: it answers
 * on MDIO address 5 (U-Boot calls it PSGMII_ID) whatever the board's front
 * panel looks like.  A board's eth-ports names the PHYs it has; the machine
 * adds this block on top of them whenever the bus is in use.
 */
#define IPQ_PSGMII_ADDR        5

typedef enum IPQBootDev {
    IPQ_BOOT_NONE = 0,
    IPQ_BOOT_SPI_NOR,           /* SPI NOR behind the QUP, flash type 6 */
    IPQ_BOOT_SPI_NAND,          /* SPI NAND behind the QUP, still type 6 */
    IPQ_BOOT_QPIC_NAND,         /* parallel NAND behind the QPIC, type 2 */
} IPQBootDev;

typedef struct IPQBoardConfig {
    bool nor;                   /* is there a NOR die on chip select 0? */
    uint32_t nor_size;          /* die bytes, power of two */
    const char *nor_id;         /* RDID as hex, e.g. "ef4019" */
    const char *nor_file;       /* the die itself (persist file) */
    const char *nor_drive;      /* ... or a block backend holding it */
    const char *nor_image;      /* content for a die that is not programmed */

    bool nand;                  /* ... and a SPI NAND on chip select 1? */
    uint32_t nand_size;
    const char *nand_id;
    const char *nand_file;
    const char *nand_drive;
    const char *nand_image;

    /*
     * ... and a *parallel* NAND behind the QPIC controller?  That is a
     * different part on a different bus: the QPIC controller drives the NAND
     * bus itself (and a BAM moves the bytes), where the SPI NAND above hangs
     * off the QUP's second chip select.  A board can have either, both or
     * neither; the hAP ac3 has a NOR and this one.
     */
    bool qpic_nand;
    uint32_t qpic_nand_size;
    const char *qpic_nand_id;
    const char *qpic_nand_file;
    const char *qpic_nand_drive;
    const char *qpic_nand_image;

    /* the TLMM pins the board wires to the dies' chip selects */
    uint32_t nor_cs_gpio;
    uint32_t nand_cs_gpio;

    /* the board's visible parts: lamps driven by pins, keys reading pins */
    const char *leds;
    const char *keys;

    /* the switch: which PHYs sit on the MDIO bus, and which port is the host's */
    const char *eth_ports;
    uint32_t eth_bridge_port;
    uint32_t eth_phy_id;

    IPQBootDev boot;            /* which die the boot chain comes from */

    /*
     * This board has no power-off register: RouterOS reaches
     * machine_power_off(), finds nothing to write, and parks the CPU in
     * machine_halt().  A parked CPU is therefore the power-off -- see
     * ipq_poweroff_poll() -- and this turns that reading off for anyone who
     * would rather keep the machine running.
     */
    bool poweroff;
} IPQBoardConfig;

/* which fields an explicit -machine property (or the env shim) provided */
enum {
    IPQ_CFG_NOR        = 1u << 0,
    IPQ_CFG_NOR_SIZE   = 1u << 1,
    IPQ_CFG_NOR_ID     = 1u << 2,
    IPQ_CFG_NOR_FILE   = 1u << 3,
    IPQ_CFG_NOR_IMAGE  = 1u << 4,
    IPQ_CFG_NAND       = 1u << 5,
    IPQ_CFG_NAND_SIZE  = 1u << 6,
    IPQ_CFG_NAND_ID    = 1u << 7,
    IPQ_CFG_NAND_FILE  = 1u << 8,
    IPQ_CFG_NAND_IMAGE = 1u << 9,
    IPQ_CFG_BOOT       = 1u << 10,
    IPQ_CFG_NOR_CS     = 1u << 11,
    IPQ_CFG_NAND_CS    = 1u << 12,
    IPQ_CFG_NOR_DRIVE  = 1u << 13,
    IPQ_CFG_NAND_DRIVE = 1u << 14,
    IPQ_CFG_LEDS       = 1u << 15,
    IPQ_CFG_KEYS       = 1u << 16,
    IPQ_CFG_ETH_PORTS  = 1u << 17,
    IPQ_CFG_ETH_BRIDGE = 1u << 18,
    IPQ_CFG_ETH_PHY_ID = 1u << 19,
    IPQ_CFG_POWEROFF   = 1u << 20,
    IPQ_CFG_QPIC_NAND       = 1u << 21,
    IPQ_CFG_QPIC_NAND_SIZE  = 1u << 22,
    IPQ_CFG_QPIC_NAND_ID    = 1u << 23,
    IPQ_CFG_QPIC_NAND_FILE  = 1u << 24,
    IPQ_CFG_QPIC_NAND_DRIVE = 1u << 25,
    IPQ_CFG_QPIC_NAND_IMAGE = 1u << 26,
};

typedef struct IPQBoardPreset {
    const char *name;
    IPQBoardConfig cfg;
} IPQBoardPreset;

/*
 * The presets.  `default` is the board this machine has always built: a
 * 32 MiB W25Q256 NOR behind chip select 0, no NAND, booting from the NOR.
 * `none` is the bare SoC -- every part has to be named on the command line.
 *
 * `mikrotik-952` is a real board, and every fact in it comes from that board's
 * own device tree (the 952G_5HacD2HnD tree inside the MikroTik dump, see
 * tools/dts2xml.py and tests/dts2xml-test.sh): a 32 MiB NOR and a 1 Gbit SPI
 * NAND on chip selects 0 and 1, five copper MDIO PHYs at addresses 0..4, and
 * three lamp lines -- user-led on pin 3, power-led on 0, the USB power line on
 * 2, active low.  The switch's PSGMII control block at address 5 is not in
 * that tree (RouterOS never addresses it) but it belongs to the switch, so
 * the machine puts it on the bus for every board that has one; see
 * IPQ_PSGMII_ADDR.
 *
 * What a preset deliberately cannot carry is a button's *behaviour*: the tree
 * says a switch is on pin 63 and another on pin 5, but not that the first one
 * should be Escape in QEMU.  That is a decision about the run, so it stays a
 * `keys=` property; the preset only knows the pins.
 */
static const IPQBoardPreset ipq_board_presets[] = {
    { "default", {
        .nor = true,
        .nor_size = IPQ_FLASH_DEFAULT,
        .nor_id = "ef4019",
        .nor_cs_gpio = 54,
        .nand = false,
        .nand_cs_gpio = 59,
        .boot = IPQ_BOOT_SPI_NOR,
    } },
    { "none", {
        .nor = false,
        .nand = false,
        .boot = IPQ_BOOT_NONE,
    } },
    { "mikrotik-952", {
        .nor = true,
        .nor_size = 33554432,
        .nor_id = "ef4019",
        .nor_cs_gpio = 54,
        .nand = true,
        .nand_size = NAND_BYTES_DEF,
        .nand_id = "c212",
        .nand_cs_gpio = 59,
        .leds = "user-led:3;power-led:0;usb-power-off:2:al",
        .eth_ports = "0;1;2;3;4",
        .boot = IPQ_BOOT_SPI_NOR,
    } },
    /*
     * MikroTik hAP ac3 (RBD53iG-5HacD2HnD-TC), from its own device tree
     * (qcom-ipq4019-hap-ac3.dts): a 16 MiB NOR and a 128 MiB *parallel* NAND
     * behind the QPIC controller.  Every pin here is a `gpios =` line in that
     * tree; the NAND is a MX30LF1G18AC (its id is what the driver prints as
     * c2 f1 80 1d), 2048+64 byte pages in 64-page blocks.
     *
     * The board boots from the NOR in this preset because that is what the
     * dumps of it in circulation are; RouterBOOT finds the rest of the
     * installation on the NAND.
     */
    { "hap-ac3", {
        .nor = true,
        .nor_size = 16777216,
        .nor_id = "ef4018",
        .nor_cs_gpio = 54,
        .qpic_nand = true,
        .qpic_nand_size = NAND_BYTES_DEF,
        .qpic_nand_id = "c2f1801d",
        .leds = "status-blue:0;status-red:1;status-green:3;wlan:23;"
                "ethernet:22;wan:28;lan1:29;lan2:30;lan3:31;lan4:32;poe:36",
        .eth_ports = "0;1;2;3;4",
        .boot = IPQ_BOOT_SPI_NOR,
    } },
};

/* what the machine was asked for, before presets and env are merged in */
static IPQBoardConfig ipq_cfg_pending;
static uint32_t ipq_cfg_set;
static const char *ipq_cfg_board;
/* the resolved answer, built once when the machine starts */
static IPQBoardConfig ipq_cfg;
static bool ipq_cfg_ready;

/* The preset names, in the order they are defined, for error messages. */
static const char *ipq_board_preset_names(void)
{
    static char names[128];
    size_t i;

    if (!names[0]) {
        for (i = 0; i < ARRAY_SIZE(ipq_board_presets); i++) {
            if (i) {
                g_strlcat(names, ", ", sizeof(names));
            }
            g_strlcat(names, ipq_board_presets[i].name, sizeof(names));
        }
    }
    return names;
}

static const IPQBoardPreset *ipq_board_preset(const char *name)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(ipq_board_presets); i++) {
        if (g_strcmp0(ipq_board_presets[i].name, name) == 0) {
            return &ipq_board_presets[i];
        }
    }
    return NULL;
}

/*
 * Hex bytes the way a datasheet writes an ID: "ef4019", "ef 40 19" and
 * "ef:40:19" all mean the same three bytes.  A plain strtoul() would swallow
 * "4019" as one number and leave the ID one byte long, so parse in pairs.
 */
static int ipq_parse_hex_bytes(const char *s, uint8_t *out, int max,
                               const char *what)
{
    const char *p = s;
    int n = 0;

    while (*p && n < max) {
        int hi, lo;

        while (*p && !g_ascii_isxdigit(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        if (!p[1] || !g_ascii_isxdigit(p[1])) {
            n = -1;
            break;
        }
        hi = g_ascii_xdigit_value(p[0]);
        lo = g_ascii_xdigit_value(p[1]);
        out[n++] = (hi << 4) | lo;
        p += 2;
    }
    if (n <= 0) {
        error_report("ipq4019: %s='%s' is not a hex byte string like ef4019",
                     what, s);
        exit(1);
    }
    return n;
}

static void ipq_board_config_resolve(MachineState *machine)
{
    const IPQBoardPreset *p;
    const char *env;
    uint32_t env_set = 0;
    IPQBoardConfig want = ipq_cfg_pending;

    if (ipq_cfg_ready) {
        return;
    }

    /* 1. the preset (board=<name>, default when nothing was said) */
    p = ipq_board_preset(ipq_cfg_board ? ipq_cfg_board : "default");
    if (!p) {
        error_report("ipq4019: no such board '%s'; known boards: %s",
                     ipq_cfg_board, ipq_board_preset_names());
        exit(1);
    }
    ipq_cfg = p->cfg;

    /* 2. explicit properties, which win over the preset */
    if (ipq_cfg_set & IPQ_CFG_NOR)        { ipq_cfg.nor        = want.nor; }
    if (ipq_cfg_set & IPQ_CFG_NOR_SIZE)   { ipq_cfg.nor_size   = want.nor_size; }
    if (ipq_cfg_set & IPQ_CFG_NOR_ID)     { ipq_cfg.nor_id     = want.nor_id; }
    if (ipq_cfg_set & IPQ_CFG_NOR_FILE)   { ipq_cfg.nor_file   = want.nor_file; }
    if (ipq_cfg_set & IPQ_CFG_NOR_DRIVE)  { ipq_cfg.nor_drive  = want.nor_drive; }
    if (ipq_cfg_set & IPQ_CFG_NOR_IMAGE)  { ipq_cfg.nor_image  = want.nor_image; }
    if (ipq_cfg_set & IPQ_CFG_NAND)       { ipq_cfg.nand       = want.nand; }
    if (ipq_cfg_set & IPQ_CFG_NAND_SIZE)  { ipq_cfg.nand_size  = want.nand_size; }
    if (ipq_cfg_set & IPQ_CFG_NAND_ID)    { ipq_cfg.nand_id    = want.nand_id; }
    if (ipq_cfg_set & IPQ_CFG_NAND_FILE)  { ipq_cfg.nand_file  = want.nand_file; }
    if (ipq_cfg_set & IPQ_CFG_NAND_DRIVE) { ipq_cfg.nand_drive = want.nand_drive; }
    if (ipq_cfg_set & IPQ_CFG_NAND_IMAGE) { ipq_cfg.nand_image = want.nand_image; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND)       { ipq_cfg.qpic_nand       = want.qpic_nand; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_SIZE)  { ipq_cfg.qpic_nand_size  = want.qpic_nand_size; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_ID)    { ipq_cfg.qpic_nand_id    = want.qpic_nand_id; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_FILE)  { ipq_cfg.qpic_nand_file  = want.qpic_nand_file; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_DRIVE) { ipq_cfg.qpic_nand_drive = want.qpic_nand_drive; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_IMAGE) { ipq_cfg.qpic_nand_image = want.qpic_nand_image; }
    if (ipq_cfg_set & IPQ_CFG_BOOT)       { ipq_cfg.boot      = want.boot; }
    if (ipq_cfg_set & IPQ_CFG_NOR_CS)     { ipq_cfg.nor_cs_gpio  = want.nor_cs_gpio; }
    if (ipq_cfg_set & IPQ_CFG_NAND_CS)    { ipq_cfg.nand_cs_gpio = want.nand_cs_gpio; }
    if (ipq_cfg_set & IPQ_CFG_LEDS)       { ipq_cfg.leds        = want.leds; }
    if (ipq_cfg_set & IPQ_CFG_KEYS)       { ipq_cfg.keys        = want.keys; }
    if (ipq_cfg_set & IPQ_CFG_ETH_PORTS)  { ipq_cfg.eth_ports   = want.eth_ports; }
    if (ipq_cfg_set & IPQ_CFG_ETH_BRIDGE) { ipq_cfg.eth_bridge_port = want.eth_bridge_port; }
    if (ipq_cfg_set & IPQ_CFG_ETH_PHY_ID) { ipq_cfg.eth_phy_id  = want.eth_phy_id; }
    if (ipq_cfg_set & IPQ_CFG_POWEROFF)   { ipq_cfg.poweroff    = want.poweroff; }

    /*
     * 3. the deprecated environment shim.  It overwrites, because that is what
     *    it did before: a harness that exports QEMU_IPQ_NAND_PERSIST expects a
     *    NAND, whatever the machine type says.
     */
    if ((env = getenv("QEMU_IPQ_FLASH_PERSIST")) && *env) {
        ipq_cfg.nor_file = env;
        env_set |= IPQ_CFG_NOR_FILE | IPQ_CFG_NOR;
        if (ipq_cfg_set & IPQ_CFG_NOR && !want.nor) {
            fprintf(stderr, "ipq4019: nor=off is overridden by "
                    "QEMU_IPQ_FLASH_PERSIST\n");
        }
        ipq_cfg.nor = true;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_IMAGE")) && *env) {
        ipq_cfg.nor_image = env;
        env_set |= IPQ_CFG_NOR_IMAGE | IPQ_CFG_NOR;
        ipq_cfg.nor = true;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_SIZE")) && *env) {
        ipq_cfg.nor_size = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NOR_SIZE;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_JEDEC")) && *env) {
        ipq_cfg.nor_id = env;
        env_set |= IPQ_CFG_NOR_ID;
    }
    if ((env = getenv("QEMU_IPQ_NAND_PERSIST")) && *env) {
        ipq_cfg.nand_file = env;
        ipq_cfg.nand = true;
        env_set |= IPQ_CFG_NAND_FILE | IPQ_CFG_NAND;
    } else if (env_true("QEMU_IPQ_NAND")) {
        ipq_cfg.nand = true;
        env_set |= IPQ_CFG_NAND;
    }
    if ((env = getenv("QEMU_IPQ_NAND_SIZE")) && *env) {
        ipq_cfg.nand_size = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NAND_SIZE;
    }
    if ((env = getenv("QEMU_IPQ_NAND_ID")) && *env) {
        ipq_cfg.nand_id = env;
        env_set |= IPQ_CFG_NAND_ID;
    }
    if ((env = getenv("QEMU_IPQ_CS_GPIO")) && *env) {
        ipq_cfg.nor_cs_gpio = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NOR_CS;
    }
    if ((env = getenv("QEMU_IPQ_NAND_CS_GPIO")) && *env) {
        ipq_cfg.nand_cs_gpio = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NAND_CS;
    }
    if ((env = getenv("QEMU_IPQ_ETH_PORTS")) && *env) {
        ipq_cfg.eth_ports = env;
        env_set |= IPQ_CFG_ETH_PORTS;
    }
    if ((env = getenv("QEMU_IPQ_ETH_BRIDGE_PORT")) && *env) {
        ipq_cfg.eth_bridge_port = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_ETH_BRIDGE;
    }
    if ((env = getenv("QEMU_IPQ_ETH_PHY_ID")) && *env) {
        ipq_cfg.eth_phy_id = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_ETH_PHY_ID;
    }
    ipq_cfg_set |= env_set;

    /* 4. defaults and sanity, so the rest of the machine can trust the struct */
    if (ipq_cfg.nor) {
        if (!ipq_cfg.nor_size) {
            ipq_cfg.nor_size = IPQ_FLASH_DEFAULT;
        }
        if (!ipq_cfg.nor_id) {
            ipq_cfg.nor_id = "ef4019";
        }
        if (!ipq_cfg.nor_cs_gpio) {
            ipq_cfg.nor_cs_gpio = 54;
        }
        if (ipq_cfg.nor_size < IPQ_FLASH_MIN ||
            ipq_cfg.nor_size > IPQ_FLASH_SIZE ||
            (ipq_cfg.nor_size & (ipq_cfg.nor_size - 1))) {
            error_report("ipq4019: nor-size=%u must be a power of two between "
                         "%u and %u bytes", ipq_cfg.nor_size,
                         (unsigned)IPQ_FLASH_MIN, (unsigned)IPQ_FLASH_SIZE);
            exit(1);
        }
    }
    if (ipq_cfg.nand) {
        if (!ipq_cfg.nand_size) {
            ipq_cfg.nand_size = NAND_BYTES_DEF;
        }
        if (!ipq_cfg.nand_id) {
            ipq_cfg.nand_id = "c212";       /* MX35LF1GE4AB */
        }
        if (!ipq_cfg.nand_cs_gpio) {
            ipq_cfg.nand_cs_gpio = 59;
        }
    }
    if (ipq_cfg.qpic_nand) {
        if (!ipq_cfg.qpic_nand_size) {
            ipq_cfg.qpic_nand_size = NAND_BYTES_DEF;
        }
        if (!ipq_cfg.qpic_nand_id) {
            ipq_cfg.qpic_nand_id = "c2f1801d";  /* MX30LF1G18AC */
        }
    }
    if (!ipq_cfg.eth_phy_id) {
        ipq_cfg.eth_phy_id = IPQ_ETH_DEFAULT_PHY_ID;
    }
    if (!(ipq_cfg_set & IPQ_CFG_POWEROFF)) {
        /* a machine whose guest cannot power it off is a machine that hangs */
        ipq_cfg.poweroff = true;
    }
    if (!ipq_cfg.eth_bridge_port) {
        /* port 0 is the CPU port: there is no cable to hand a host frame to */
        ipq_cfg.eth_bridge_port = 5;
    }
    if (ipq_cfg.eth_bridge_port > 6) {
        error_report("ipq4019: eth-bridge-port=%u must be 1..6",
                     ipq_cfg.eth_bridge_port);
        exit(1);
    }
    /* its geometry is checked where the part is built (ipq_board_add_nand) */

    /* the boot device has to exist: that is the whole point of naming it */
    if (ipq_cfg.boot != IPQ_BOOT_NONE) {
        bool have = ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? ipq_cfg.nor :
                    ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? ipq_cfg.nand :
                    ipq_cfg.qpic_nand;

        if (!have) {
            error_report("ipq4019: boot-device=%s but no %s die is configured "
                         "(add %s=on, or pick another boot device)",
                         ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "spi-nor" :
                         ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "spi-nand" :
                         "qpic-nand",
                         ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "NOR" :
                         ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "SPI NAND" :
                         "parallel NAND",
                         ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "nor" :
                         ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "nand" :
                         "qpic-nand");
            exit(1);
        }
    } else if (ipq_cfg.nor || ipq_cfg.nand || ipq_cfg.qpic_nand) {
        /* a board with dies but no boot strap: the NOR is what a strap does */
        ipq_cfg.boot = ipq_cfg.nor ? IPQ_BOOT_SPI_NOR :
                       ipq_cfg.nand ? IPQ_BOOT_SPI_NAND : IPQ_BOOT_QPIC_NAND;
    }

    /*
     * -kernel is content, not wiring: it fills the boot device's die.  A
     * machine with no boot device at all cannot use it, and says so instead of
     * quietly inventing a NOR the way it used to.
     */
    if (machine->kernel_filename && ipq_cfg.boot == IPQ_BOOT_NONE) {
        error_report("ipq4019: -kernel %s given, but this board has no boot "
                     "device to put it in (board=none); add nor=on or "
                     "nand=on, or use -machine board=default",
                     machine->kernel_filename);
        exit(1);
    }
    if (machine->kernel_filename && !ipq_cfg.nor_image && !ipq_cfg.nand_image
        && !ipq_cfg.qpic_nand_image) {
        if (ipq_cfg.boot == IPQ_BOOT_SPI_NOR) {
            ipq_cfg.nor_image = machine->kernel_filename;
        } else if (ipq_cfg.boot == IPQ_BOOT_SPI_NAND) {
            ipq_cfg.nand_image = machine->kernel_filename;
        } else if (ipq_cfg.boot == IPQ_BOOT_QPIC_NAND) {
            ipq_cfg.qpic_nand_image = machine->kernel_filename;
        }
    }

    fprintf(stderr, "ipq4019: board %s: nor ", ipq_cfg_board ? ipq_cfg_board
            : "default");
    if (ipq_cfg.nor) {
        fprintf(stderr, "%u MiB cs gpio %u%s, ",
                (unsigned)(ipq_cfg.nor_size / MiB), ipq_cfg.nor_cs_gpio,
                (ipq_cfg.nor_file || ipq_cfg.nor_drive) ? " (store)" : "");
    } else {
        fprintf(stderr, "none, ");
    }
    fprintf(stderr, "nand ");
    if (ipq_cfg.nand) {
        fprintf(stderr, "%u MiB cs gpio %u%s, ",
                (unsigned)(ipq_cfg.nand_size / MiB), ipq_cfg.nand_cs_gpio,
                (ipq_cfg.nand_file || ipq_cfg.nand_drive) ? " (store)" : "");
    } else {
        fprintf(stderr, "none, ");
    }
    fprintf(stderr, "qpic-nand ");
    if (ipq_cfg.qpic_nand) {
        if (ipq_cfg.qpic_nand_size % MiB) {
            fprintf(stderr, "%u bytes", ipq_cfg.qpic_nand_size);
        } else {
            fprintf(stderr, "%u MiB", (unsigned)(ipq_cfg.qpic_nand_size / MiB));
        }
        fprintf(stderr, "%s, ",
                (ipq_cfg.qpic_nand_file || ipq_cfg.qpic_nand_drive) ?
                " (store)" : "");
    } else {
        fprintf(stderr, "none, ");
    }
    fprintf(stderr, "boot-device %s\n",
            ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "spi-nor" :
            ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "spi-nand" :
            ipq_cfg.boot == IPQ_BOOT_QPIC_NAND ? "qpic-nand" : "none");

    ipq_cfg_ready = true;
}

/* ------------------------------------------------------------------ */
/* generic shadowed stub                                               */
/* ------------------------------------------------------------------ */

struct IPQStub {
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
};

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
    static char buf[32];
    CPUState *cs = current_cpu;

    if (!cs) {
        return "";
    }
    snprintf(buf, sizeof(buf), " pc=%08x",
             (uint32_t)ARM_CPU(cs)->env.regs[15]);
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

/* QEMU_IPQ_STATE_DUMP=<path>: dump the whole model state at the handoff. */
static void ipq_state_dump(const char *tag);
static void state_note_tx(const uint8_t *buf, unsigned n);

/*
 * Modelled register files that keep software's writes (the ESS block, for
 * now).  They are not IPQStub, so the stub sweep does not reach them, but a
 * system reset has to clear them for the same reason: the next boot prints
 * "Switch id detected ..." out of an id field the previous OS already
 * rewrote.
 */

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
struct IPQPsHold {
    uint32_t val;
};


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

static void ipq_poweroff_start(void)
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

struct IPQWdt {
    MemoryRegion mr;
    uint32_t regs[0x1000 / 4];
    QEMUTimer *bark;
    QEMUTimer *bite;
};


static uint64_t wdt_read(void *opaque, hwaddr off, unsigned size)
{
    IPQWdt *w = opaque;

    return w->regs[(off & 0xfff) / 4];
}

static void wdt_disarm(IPQWdt *w)
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
static uint64_t ipq_dram_size;

static void ipq_reset_dump(void *opaque)
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
struct IPQMem {
    MemoryRegion mr;
    const char *name;
    uint8_t *data;
    uint64_t size;
};

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

struct IPQQupl {
    MemoryRegion mr;
    uint32_t shadow[0x1000 / 4];

    /* SPI slave state machine (driven by every byte clocked out) */
    int phase;              /* 0 opcode, 1 address, 2 dummy, 3 data */
    uint8_t opcode;
    uint32_t addr;
    int addr_left;
    int dummy_left;
    int cs;                 /* a die is selected (FORCE_CS or a cs-gpio) */
    int cs_force;           /* QUP_IO_CONTROL FORCE_CS (QCA boot drivers) */
    int cs_owner;           /* CS_OWNER_*: what selected the current die */
    int cs_gpio_die;        /* die held by a cs-gpio (CS_OWNER_GPIO) */
    int target;             /* which die the pins selected: NAND_CS_* */
    int nand_read;          /* stream the NAND page cache (READ FROM CACHE) */
    int read_data;          /* stream data out of the flash */
    uint32_t rx_off;        /* next flash offset of the data stream */
    uint8_t rx_buf[8];      /* short replies: JEDEC id, status */
    int rx_len;
    int rx_pos;
    int rx_wrap;            /* the reply repeats instead of ending (RDID) */
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
};

static uint32_t ipq_flash_die_size = IPQ_FLASH_DEFAULT;
static uint8_t ipq_flash_jedec[4] = { 0xef, 0x40, 0x19, 0x00 };
static int ipq_flash_jedec_len = 3;

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

/*
 * The guest's PC for a log line.  current_cpu is NULL when the access comes
 * from the monitor, the gdb stub or a DMA engine rather than from a vCPU, and
 * a debug line must never be the thing that crashes the machine.
 */
static uint32_t die_dbg_pc(void)
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
#define QN_CFG0_CW_PER_PAGE     GENMASK(8, 6)

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

struct IPQQpic {
    MemoryRegion mr;
    IPQParNand *die;

    uint32_t reg[IPQ_QPIC_NANDC_SIZE / 4];

    uint8_t buf[NAND_STRIDE];   /* the controller's page buffer */
    uint32_t in_len;            /* program bytes the data pipe delivered */
    uint32_t wr_col;            /* column the pending program starts at */
    uint32_t rd_pos;            /* read stream cursor inside buf */
    uint32_t last_page;         /* page the stream belongs to */
    bool have_page;
    bool program_pending;
};

static bool ipq_qpic_store_loaded;

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
        d->data[off + col + i] &= buf[i];
    }
    qpic_die_persist(d, d->data + off, off, NAND_STRIDE);
}

/* ---- the controller --------------------------------------------------- */

/*
 * What one EXEC exports to the data pipe.
 *
 * A page read is issued once per codeword -- the driver rewrites CMD and EXEC
 * for each step of the same page -- so the controller keeps one cursor over
 * the raw page (data and spare in die order) and each EXEC pulls from where
 * the previous one stopped.  That is the observable behaviour of the
 * read-location registers for the one-location-per-step sequence both
 * RouterBOOT and the mainline driver produce, without inventing a codeword
 * layout the register set does not carry.
 */
static uint32_t qpic_pull(IPQQpic *q, uint8_t *dst, uint32_t len)
{
    uint32_t n = 0;

    while (n < len) {
        uint32_t take;

        if (q->rd_pos >= NAND_STRIDE) {
            q->rd_pos = 0;
        }
        take = MIN(len - n, (uint32_t)NAND_STRIDE - q->rd_pos);
        memcpy(dst + n, q->buf + q->rd_pos, take);
        q->rd_pos += take;
        n += take;
    }
    return n;
}

static void qpic_push(IPQQpic *q, const uint8_t *src, uint32_t len)
{
    while (len && q->in_len < NAND_STRIDE) {
        uint32_t take = MIN(len, (uint32_t)NAND_STRIDE - q->in_len);

        memcpy(q->buf + q->in_len, src, take);
        q->in_len += take;
        src += take;
        len -= take;
    }
}

static void qpic_commit_program(IPQQpic *q)
{
    if (!q->program_pending) {
        return;
    }
    q->program_pending = false;
    qpic_die_program(q->die, q->last_page, q->wr_col, q->buf, q->in_len);
    q->in_len = 0;
    ipq_die_store_sync(&q->die->store);
    q->have_page = false;
    q->last_page = UINT32_MAX;
}

static void qpic_exec(IPQQpic *q)
{
    uint32_t cmd = q->reg[QN_CMD / 4];
    uint32_t op = cmd & QN_CMD_OP_MASK;
    uint32_t page = ((q->reg[QN_ADDR1 / 4] & 0xff) << 16) |
                    ((q->reg[QN_ADDR0 / 4] >> 16) & 0xffff);

    /*
     * The command pipe and the data pipe are independent: EXEC can arrive
     * before the page bytes do.  A program that was left pending is committed
     * by whichever command comes next -- normally the status poll -- and by
     * the time it is committed the data long since landed.
     */
    if (q->program_pending && q->in_len) {
        qpic_commit_program(q);
    }

    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        qpic_log(
                      "qpic: exec op=%u page=%u col=%u cmd=%08x addr0=%08x "
                      "addr1=%08x cfg0=%08x ecc=%08x loc=%08x in=%u\n",
                      op, page, q->reg[QN_ADDR0 / 4] & 0xffff, cmd,
                      q->reg[QN_ADDR0 / 4], q->reg[QN_ADDR1 / 4],
                      q->reg[QN_DEV0_CFG0 / 4], q->reg[QN_DEV0_ECC_CFG / 4],
                      q->reg[QN_RD_LOC0 / 4], q->in_len);
    }

    switch (op) {
    case QN_OP_RESET_DEVICE:
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
        qpic_die_erase(q->die, page);
        /*
         * The page buffer holds what the last read put there; the array has
         * just changed underneath it, so the next read of the same page has to
         * fetch again.  Without this a read-after-erase (RouterBOOT verifies
         * its own writes that way) would answer with the erased page's previous
         * contents.
         */
        q->have_page = false;
        q->last_page = UINT32_MAX;
        break;

    case QN_OP_PAGE_READ:
    case QN_OP_PAGE_READ_ECC:
    case QN_OP_PAGE_READ_SPARE:
    case QN_OP_ONFI_READ:
        if (!q->have_page || page != q->last_page) {
            q->last_page = page;
            q->have_page = true;
            /*
             * Where in the page the stream starts is the opcode and the column
             * together: a normal read addresses the data area, a spare read
             * addresses the spare area, whose column 0 is offset 2048 of the
             * raw page.  RouterBOOT looks for its per-block headers that way
             * (op=4, column 0, one page per block) and the vendor driver reads
             * the ONFI/parameter bytes at a column; serving both from offset 0
             * gave them page data instead of what they asked for, which is a
             * silently wrong read, not a failed one.
             */
            q->rd_pos = (op == QN_OP_PAGE_READ_SPARE ? NAND_PAGE : 0) +
                        MIN(q->reg[QN_ADDR0 / 4] & 0xffffu,
                            (uint32_t)NAND_STRIDE - 1);
            if (page < q->die->pages) {
                memcpy(q->buf, q->die->data + (size_t)page * NAND_STRIDE,
                       NAND_STRIDE);
            } else {
                memset(q->buf, 0xff, NAND_STRIDE);
            }
        }
        /* A read that follows a program of the same page commits it first. */
        qpic_commit_program(q);
        break;

    case QN_OP_PROGRAM_PAGE:
    case QN_OP_PROGRAM_PAGE_ECC:
    case QN_OP_PROGRAM_SPARE:
        q->last_page = page;
        q->wr_col = q->reg[QN_ADDR0 / 4] & 0xffff;
        if (q->in_len) {
            qpic_die_program(q->die, page, q->wr_col, q->buf, q->in_len);
            q->in_len = 0;
            ipq_die_store_sync(&q->die->store);
            q->have_page = false;
            q->last_page = UINT32_MAX;
        } else {
            /*
             * The engine and the data channel are independent: the command
             * pipe can reach EXEC before the data has landed.  Remember the
             * program and let the data commit it when it arrives.
             */
            q->program_pending = true;
        }
        break;

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
         */
        return 0x20;
    }
    if (off == QN_BUFFER_STATUS) {
        return 0;                               /* no ECC errors */
    }
    if (off == QN_VERSION) {
        return 0x00020000u;                     /* QPIC v2.0 */
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
    /*
     * Off by default.  A level line whose source the consumer does not clear
     * is worse than no line at all: MikroTik's netinstall kernel enabled the
     * pipe source and then never acknowledged it, and the asserted line turned
     * a failed probe into "soft lockup - CPU#0 stuck for 22s".  The BAM's
     * status registers are all here, so QEMU_IPQ_QPIC_IRQ=1 is the way to try
     * a driver that really wants the interrupt; until one is shown to work,
     * the default is the polling behaviour every bootloader in this tree uses.
     */
    if (b->irq && env_true("QEMU_IPQ_QPIC_IRQ")) {
        qemu_set_irq(b->irq, qpic_bam_irq_pending(b) != 0);
    }
}

static void qpic_bam_data_in(IPQQpic *q, uint32_t addr, uint32_t len)
{
    uint8_t chunk[512];
    uint32_t done = 0;

    while (done < len) {
        uint32_t take = MIN(len - done, (uint32_t)sizeof(chunk));

        address_space_read(&address_space_memory, addr + done,
                           MEMTXATTRS_UNSPECIFIED, chunk, take);
        qpic_push(q, chunk, take);
        done += take;
    }
}

static void qpic_bam_data_out(IPQQpic *q, uint32_t addr, uint32_t len)
{
    uint8_t chunk[512];
    uint32_t done = 0;

    while (done < len) {
        uint32_t take = MIN(len - done, (uint32_t)sizeof(chunk));

        qpic_pull(q, chunk, take);
        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            qpic_log(
                          "qpic: out %08x +%u from page %u pos %u: "
                          "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                          addr + done, take, q->last_page,
                          q->rd_pos - take,
                          chunk[0], take > 1 ? chunk[1] : 0,
                          take > 2 ? chunk[2] : 0, take > 3 ? chunk[3] : 0,
                          take > 4 ? chunk[4] : 0, take > 5 ? chunk[5] : 0,
                          take > 6 ? chunk[6] : 0, take > 7 ? chunk[7] : 0);
        }
        address_space_write(&address_space_memory, addr + done,
                            MEMTXATTRS_UNSPECIFIED, chunk, take);
        done += take;
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
        if (c == 1) {       /* register -> memory (mask = upper address bits) */
            uint64_t dst = (uint64_t)hi << 32 | data;
            uint32_t v = qpic_reg_read(q, off);

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
        while (p->cons != tail && guard++ < 4096) {
            uint32_t d[2];

            address_space_read(&address_space_memory,
                               p->desc_fifo + p->cons * 8,
                               MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
            if (d[1] & 0xffff) {
                if (n == QB_PIPE_CMD) {
                    qpic_bam_ce(b, d[0], d[1] & 0xffff);
                } else if (p->ctrl & QB_P_DIRECTION) {
                    if (env_true("QEMU_IPQ_QPIC_LOG")) {
                        qpic_log(
                                      "qpic: rx desc %08x len %u\n",
                                      d[0], d[1] & 0xffff);
                    }
                    qpic_bam_data_out(q, d[0], d[1] & 0xffff);
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
    }
    p->irq_stts |= QB_P_IRQ_PRCSD | QB_P_IRQ_TRNSFR_END;
    p->irq_stts &= ~QB_P_IRQ_OUT_DESC;
    qpic_bam_update_irq(b);
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
    switch (off) {
    case QB_CTRL:        return b->ctrl;
    case QB_DESC_TRSHLD: return b->desc_trshld;
    case QB_CNFG_BITS:   return b->cfg_bits;
    case QB_IRQ_STTS:    return b->irq_stts;
    case QB_IRQ_EN:      return b->irq_en;
    case QB_REVISION:    return 0x00010007u;    /* v1.7 */
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
        qpic_log("qpic-bam: write %#x <- %08x\n",
                      (uint32_t)off, (uint32_t)val);
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
 * previous boot left *inside the controller*: a program left pending must not
 * be committed by the next boot's first command (that would put the old page
 * buffer into the die at a page nobody asked for), and the read cursor has to
 * start over, or the first page read of the new boot would continue the old
 * one instead of starting at page 0.
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
    q->in_len = 0;
    q->wr_col = 0;
    q->rd_pos = 0;
    q->last_page = UINT32_MAX;
    q->have_page = false;
    q->program_pending = false;

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
static void qup_clock(IPQQupl *q, uint8_t mosi)
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
 * chip select.  The vendor DTS (the netinstall kernel's) describes both as
 *
 *     cs-gpios = <&tlmm 54 0>, <&tlmm 59 0>;    (both GPIO_ACTIVE_HIGH)
 *
 * and its pinctrl state asks for `output-high` on the same two pins, so
 * modelling the IO register is what gives each die its chip-select edges:
 * without it every QUP state RESET would have to be guessed as a transaction
 * boundary, and one SPI message with a separate command and data transfer
 * (which is exactly how spi-mem issues a read) would be chopped in half.
 *
 * Note that Linux still programs SPI_IO_CONTROL's FORCE_CS as well, because
 * spi_qup_set_cs() is installed even when the controller uses gpio
 * descriptors -- with the opposite sense to QCA's boot drivers.  See
 * qup_cs_force().
 *
 * The rest of the block only needs to read back what was written: gpiolib
 * does read-modify-writes on these registers.
 */
#define IPQ_TLMM_IO_OFFSET   0x004u
#define IPQ_TLMM_IO_OUT_BIT  1

/*
 * Reset value of the whole block: all ones.  Every pin reads back as "no
 * function selected, no pull, output released", which is what a cold boot
 * shows and what the boot ROM's own pin dump expects -- the booter prints
 *
 *     nand: cs54 cfg FFFFFFFF->200
 *
 * on a cold boot and must see the same value after a reboot: the previous OS
 * leaves pin 54 muxed to its SPI function and pulled up (0xFFFFFE00 here), so
 * a machine that forgets to reset the block hands the next boot a pin
 * controller that is still programmed by Linux.
 */
#define IPQ_TLMM_RESET_VAL   0xffffffffu

/*
 * The pins.
 *
 * A board part either drives a pin or watches one, and both are per-pin lines
 * here -- the way a qdev GPIO controller exposes them -- so a board device
 * never has to know about TLMM register layout.  The line's level is the bit
 * the guest drives (GPIO_IO bit 1, high = 1), and a handler is told about
 * *every* write to its pin's IO register, not only about changes: gpiolib
 * takes and releases a chip select around each message, and asserting the same
 * die twice in a row is two transaction boundaries.
 */
#define IPQ_TLMM_NPINS 128

typedef void (*IPQPinHandler)(void *opaque, int level);

struct IPQTLMM {
    MemoryRegion mr;
    uint32_t *regs;
    uint32_t nregs;

    uint8_t out_level[IPQ_TLMM_NPINS];          /* what the guest drives */
    IPQPinHandler handler[IPQ_TLMM_NPINS];
    void *handler_opaque[IPQ_TLMM_NPINS];

    /*
     * Pins the board drives into the SoC (buttons, PHY interrupts): their
     * level is what a read of the pin reports, instead of the latch the guest
     * last wrote.
     */
    uint8_t in_level[IPQ_TLMM_NPINS];
    bool in_used[IPQ_TLMM_NPINS];
};

static bool ipq_tlmm_pin_mapped(IPQTLMM *t, unsigned pin)
{
    return pin < IPQ_TLMM_NPINS &&
           pin * 0x1000 + IPQ_TLMM_IO_OFFSET + 4 <= t->nregs * 4;
}

/* the level the guest is driving onto a pin, from the IO register's latch */
static int ipq_tlmm_latch(IPQTLMM *t, unsigned pin)
{
    uint32_t v = t->regs[(pin * 0x1000 + IPQ_TLMM_IO_OFFSET) >> 2];

    return (v >> IPQ_TLMM_IO_OUT_BIT) & 1;
}

/*
 * A write to a pin's IO register.  Called on every write, not only on a
 * change, and after the shadow update so the handler sees the new level.
 */
static void ipq_tlmm_pin_written(IPQTLMM *t, unsigned pin)
{
    int level;

    if (!ipq_tlmm_pin_mapped(t, pin) || !t->handler[pin]) {
        return;
    }
    level = ipq_tlmm_latch(t, pin);
    t->out_level[pin] = level;
    t->handler[pin](t->handler_opaque[pin], level);
}

/* Attach a board part to a pin's output: what the guest drives, it sees. */
static void ipq_tlmm_connect(IPQTLMM *t, unsigned pin, IPQPinHandler fn,
                             void *opaque)
{
    if (pin >= IPQ_TLMM_NPINS) {
        error_report("ipq4019: pin %u is outside the TLMM frame", pin);
        exit(1);
    }
    t->handler[pin] = fn;
    t->handler_opaque[pin] = opaque;
    t->out_level[pin] = ipq_tlmm_pin_mapped(t, pin) ? ipq_tlmm_latch(t, pin) : 1;
}

/*
 * The board drives a pin -- a button, a PHY interrupt -- from now on a read of
 * the pin's IO register reports this level instead of the latch the guest
 * wrote.  A pressed key is a level, not an event: the driver polls it, or would
 * be wired to an interrupt this machine does not model.
 */
static void ipq_tlmm_set_pin_in(IPQTLMM *t, unsigned pin, int level)
{
    if (pin >= IPQ_TLMM_NPINS) {
        return;
    }
    t->in_used[pin] = true;
    t->in_level[pin] = level ? 1 : 0;
}

/* What the guest is driving onto a pin right now (1 = released, i.e. high). */
static int ipq_tlmm_out_level(IPQTLMM *t, unsigned pin)
{
    return pin < IPQ_TLMM_NPINS ? t->out_level[pin] : 1;
}

/*
 * A chip select was driven.  What decides is the *level*, not the edge and
 * not the rest of the register:
 *
 *   - cs-gpios = <&tlmm 54 0>, <&tlmm 59 0> declares both selects
 *     GPIO_ACTIVE_HIGH and the node does not set SPI_CS_HIGH, so the SPI core
 *     drives a select LOW and releases it HIGH (spi_set_cs() inverts before
 *     handing the value to gpiod).  The pinctrl state agrees: mux_cs asks for
 *     `output-high`, i.e. every select is released at boot.
 *   - The pin is therefore asserted while the TLMM IO register's output latch
 *     (bit 1) is 0.  Everything else in that register is pinctrl's business:
 *     mux_cs programs the pins with a read-modify-write of a reset value of all
 *     ones, so a release legitimately lands as 0xffffffff and an assertion as
 *     0xfffffffd.
 *
 * This is the same rule the machine used when it read the register itself; the
 * difference is that it is now a line on the SoC's pin controller instead of a
 * special case keyed on two hardcoded pin numbers.
 */
static void ipq_cs_line(void *opaque, int level)
{
    unsigned which = (unsigned)(uintptr_t)opaque;
    IPQQupl *q = ipq->qup_spi;
    bool asserted = level == 0;

    if (!q) {
        return;
    }
    if (die_dbg_on()) {
        die_dbg_log("die: cs%u %s %s\n",
                    which == NAND_CS_NAND ? ipq_cfg.nand_cs_gpio
                                          : ipq_cfg.nor_cs_gpio,
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

static uint64_t tlmm_read(void *opaque, hwaddr off, unsigned size)
{
    IPQTLMM *t = opaque;
    uint32_t idx = off >> 2;
    unsigned pin = off >> 12;
    uint32_t v = idx < t->nregs ? t->regs[idx] : 0xffffffffu;

    /*
     * A pin the board drives reads back the board's level, not the latch the
     * guest last wrote there: that is what makes a button visible to the
     * driver instead of a stale copy of its own output.
     */
    if ((off & 0xfff) == IPQ_TLMM_IO_OFFSET && pin < IPQ_TLMM_NPINS &&
        t->in_used[pin]) {
        v = (v & ~(1u << IPQ_TLMM_IO_OUT_BIT)) |
            ((uint32_t)t->in_level[pin] << IPQ_TLMM_IO_OUT_BIT);
    }
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
    if ((off & 0xfff) == IPQ_TLMM_IO_OFFSET) {
        ipq_tlmm_pin_written(t, pin);
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
        t->regs[i] = IPQ_TLMM_RESET_VAL;
    }
    memory_region_init_io(&t->mr, NULL, &tlmm_ops, t, name, size);
    memory_region_add_subregion(parent, base, &t->mr);
    ipq->tlmm = t;
}

/* A system reset puts every pin back to its reset value. */
static void tlmm_dev_reset(IPQTLMM *t)
{
    uint32_t i;

    for (i = 0; i < t->nregs; i++) {
        t->regs[i] = IPQ_TLMM_RESET_VAL;
    }
}


/* ------------------------------------------------------------------ */
/* the board's face: lamps a person can see, keys they can press        */
/* ------------------------------------------------------------------ */

/*
 * An LED and a button are the cheapest parts on a router and the hardest to
 * get at from a terminal.  One is an output pin with a lamp on it, the other an
 * input pin with a switch on it -- board wiring, not SoC, so both are named in
 * the machine configuration:
 *
 *   -machine ipq4019,leds=status:5:al;wlan:60
 *   -machine ipq4019,keys=reset:61:al:esc;wps:59:al:f
 *
 * The lamp is shown in QEMU's own window: this file registers a display
 * console with one labelled lamp per LED, which is the same mechanism a
 * display adapter uses and needs no second management interface.  The key is a
 * QemuInputHandler: a keypress in that window becomes a level on its pin, and
 * because the handler is not bound to a particular console, QMP's send-key
 * presses it headlessly too (`send-key esc`).
 *
 * Both are glue.  The pins are IPQTLMM lines, and the machine already gives a
 * board part "the level of pin N, on every write to it"; an LED only has to
 * remember that level.  A key sets the level a read of the pin reports.
 */

/* a 6x8 bitmap font, because a panel needs labels and QEMU has no font */
static const uint8_t ipq_font6x8[][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ' ' */
    { 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x08, 0x00 }, /* '!' */
    { 0x14, 0x14, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* '"' */
    { 0x0a, 0x00, 0x1e, 0x14, 0x3e, 0x14, 0x00, 0x00 }, /* '#' */
    { 0x1c, 0x1a, 0x18, 0x0c, 0x0a, 0x3a, 0x1c, 0x08 }, /* '$' */
    { 0x32, 0x2c, 0x34, 0x08, 0x0e, 0x14, 0x06, 0x00 }, /* '%' */
    { 0x0c, 0x14, 0x1c, 0x1a, 0x2e, 0x24, 0x1e, 0x00 }, /* '&' */
    { 0x08, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ''' */
    { 0x04, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08 }, /* '(' */
    { 0x08, 0x08, 0x0c, 0x04, 0x04, 0x04, 0x0c, 0x08 }, /* ')' */
    { 0x08, 0x1c, 0x0c, 0x04, 0x00, 0x00, 0x00, 0x00 }, /* '*' */
    { 0x00, 0x08, 0x08, 0x3e, 0x08, 0x08, 0x00, 0x00 }, /* '+' */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x18 }, /* ',' */
    { 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00 }, /* '-' */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00 }, /* '.' */
    { 0x02, 0x04, 0x04, 0x08, 0x08, 0x10, 0x10, 0x00 }, /* '/' */
    { 0x1c, 0x16, 0x12, 0x1e, 0x12, 0x16, 0x1c, 0x00 }, /* '0' */
    { 0x0c, 0x1c, 0x0c, 0x0c, 0x0c, 0x0c, 0x1e, 0x00 }, /* '1' */
    { 0x1c, 0x16, 0x06, 0x0c, 0x18, 0x10, 0x1e, 0x00 }, /* '2' */
    { 0x1c, 0x16, 0x06, 0x0c, 0x06, 0x16, 0x1c, 0x00 }, /* '3' */
    { 0x04, 0x0c, 0x14, 0x14, 0x3e, 0x04, 0x04, 0x00 }, /* '4' */
    { 0x1e, 0x10, 0x1c, 0x16, 0x02, 0x16, 0x1c, 0x00 }, /* '5' */
    { 0x0c, 0x16, 0x10, 0x1c, 0x16, 0x16, 0x0c, 0x00 }, /* '6' */
    { 0x1e, 0x06, 0x04, 0x0c, 0x08, 0x08, 0x08, 0x00 }, /* '7' */
    { 0x1c, 0x16, 0x16, 0x1c, 0x16, 0x16, 0x1c, 0x00 }, /* '8' */
    { 0x1c, 0x16, 0x16, 0x1e, 0x06, 0x16, 0x1c, 0x00 }, /* '9' */
    { 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00 }, /* ':' */
    { 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08 }, /* ';' */
    { 0x00, 0x02, 0x1c, 0x30, 0x1c, 0x02, 0x00, 0x00 }, /* '<' */
    { 0x00, 0x00, 0x3e, 0x00, 0x3e, 0x00, 0x00, 0x00 }, /* '=' */
    { 0x00, 0x30, 0x0c, 0x06, 0x0c, 0x30, 0x00, 0x00 }, /* '>' */
    { 0x1c, 0x36, 0x06, 0x04, 0x08, 0x00, 0x08, 0x00 }, /* '?' */
    { 0x0c, 0x12, 0x2e, 0x34, 0x34, 0x3e, 0x00, 0x1c }, /* '@' */
    { 0x0c, 0x0c, 0x1c, 0x14, 0x1e, 0x32, 0x22, 0x00 }, /* 'A' */
    { 0x1c, 0x16, 0x16, 0x1c, 0x12, 0x12, 0x1e, 0x00 }, /* 'B' */
    { 0x1c, 0x16, 0x30, 0x30, 0x30, 0x12, 0x1c, 0x00 }, /* 'C' */
    { 0x00, 0x1c, 0x16, 0x12, 0x12, 0x16, 0x1c, 0x00 }, /* 'D' */
    { 0x1e, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1e, 0x00 }, /* 'E' */
    { 0x1e, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10, 0x00 }, /* 'F' */
    { 0x1c, 0x16, 0x30, 0x36, 0x32, 0x12, 0x1c, 0x00 }, /* 'G' */
    { 0x16, 0x16, 0x16, 0x1e, 0x16, 0x16, 0x16, 0x00 }, /* 'H' */
    { 0x1e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1e, 0x00 }, /* 'I' */
    { 0x0e, 0x06, 0x06, 0x06, 0x06, 0x14, 0x1c, 0x00 }, /* 'J' */
    { 0x16, 0x14, 0x1c, 0x1c, 0x1c, 0x16, 0x12, 0x00 }, /* 'K' */
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1e, 0x00 }, /* 'L' */
    { 0x36, 0x36, 0x3e, 0x3e, 0x2a, 0x22, 0x22, 0x00 }, /* 'M' */
    { 0x12, 0x1a, 0x1a, 0x1a, 0x1e, 0x16, 0x16, 0x00 }, /* 'N' */
    { 0x1c, 0x16, 0x32, 0x32, 0x32, 0x16, 0x1c, 0x00 }, /* 'O' */
    { 0x1c, 0x16, 0x12, 0x16, 0x1c, 0x10, 0x10, 0x00 }, /* 'P' */
    { 0x1c, 0x16, 0x32, 0x32, 0x32, 0x16, 0x1c, 0x0c }, /* 'Q' */
    { 0x1c, 0x12, 0x12, 0x1c, 0x14, 0x16, 0x12, 0x00 }, /* 'R' */
    { 0x1c, 0x16, 0x10, 0x1c, 0x06, 0x32, 0x1c, 0x00 }, /* 'S' */
    { 0x3e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00 }, /* 'T' */
    { 0x12, 0x12, 0x12, 0x12, 0x16, 0x16, 0x1c, 0x00 }, /* 'U' */
    { 0x32, 0x32, 0x16, 0x14, 0x1c, 0x0c, 0x0c, 0x00 }, /* 'V' */
    { 0x22, 0x22, 0x2a, 0x3e, 0x1e, 0x16, 0x16, 0x00 }, /* 'W' */
    { 0x32, 0x14, 0x0c, 0x0c, 0x1c, 0x16, 0x32, 0x00 }, /* 'X' */
    { 0x32, 0x16, 0x1c, 0x0c, 0x08, 0x08, 0x08, 0x00 }, /* 'Y' */
    { 0x1e, 0x06, 0x0c, 0x08, 0x18, 0x10, 0x3e, 0x00 }, /* 'Z' */
};


#define IPQ_PANEL_BG        0x101418u
#define IPQ_PANEL_FRAME     0x2c343cu
#define IPQ_PANEL_ON        0x38e070u
#define IPQ_PANEL_OFF       0x1c2830u
#define IPQ_PANEL_TEXT      0xa8b4c0u

#define IPQ_PANEL_PAD       10
#define IPQ_PANEL_ROW       20
#define IPQ_PANEL_LAMP      14
#define IPQ_PANEL_SCALE     2
#define IPQ_PANEL_ADVANCE   (6 * IPQ_PANEL_SCALE + 2)

typedef struct IPQLedPanel {
    DeviceState parent;

    QemuConsole *con;
    IPQLed *leds;               /* the board's array, not owned here */
    unsigned n;
    int w, h, label_x;
} IPQLedPanel;

#define TYPE_IPQ_LED_PANEL "ipq4019-led-panel"
OBJECT_DECLARE_SIMPLE_TYPE(IPQLedPanel, IPQ_LED_PANEL)

/*
 * One rectangle of colour, in whatever pixel format the UI chose.  32bpp is
 * what every graphical front end uses and takes the fast path; the rest is
 * written by hand so the panel is not silently blank on a 16bpp surface.
 */
static void ipq_panel_fill(DisplaySurface *sf, int x, int y, int w, int h,
                           uint32_t rgb)
{
    int bpp = surface_bits_per_pixel(sf);
    int stride = surface_stride(sf);
    uint8_t *data = surface_data(sf);
    int i, j;

    if (!data || bpp < 15) {
        return;
    }
    if (bpp == 32) {
        pixman_fill((uint32_t *)data, stride / 4, 32, x, y, w, h,
                    0xff000000u | rgb);
        return;
    }
    for (j = y; j < y + h; j++) {
        for (i = x; i < x + w; i++) {
            uint8_t *p = data + j * stride + i * (bpp / 8);

            switch (bpp) {
            case 24:
                p[0] = rgb & 0xff;
                p[1] = (rgb >> 8) & 0xff;
                p[2] = (rgb >> 16) & 0xff;
                break;
            case 16:
            case 15: {
                uint16_t v = ((rgb >> 19) & 0x1f) << 11 |
                             ((rgb >> 10) & 0x3f) << 5 |
                             ((rgb >> 3) & 0x1f);

                memcpy(p, &v, 2);
                break;
            }
            default:
                break;
            }
        }
    }
}

/* Text, one glyph at a time, twice the font's size: readable at arm's length. */
static void ipq_panel_text(DisplaySurface *sf, int x, int y, const char *text,
                           uint32_t rgb)
{
    for (; *text; text++, x += IPQ_PANEL_ADVANCE) {
        unsigned c = g_ascii_toupper(*text);
        const uint8_t *g;
        int row, col;

        if (c < 32 || c > 90) {
            c = '?';
        }
        g = ipq_font6x8[c - 32];
        for (row = 0; row < 8; row++) {
            for (col = 0; col < 6; col++) {
                if (g[row] & (1 << (5 - col))) {
                    ipq_panel_fill(sf, x + col * IPQ_PANEL_SCALE,
                                   y + row * IPQ_PANEL_SCALE,
                                   IPQ_PANEL_SCALE, IPQ_PANEL_SCALE, rgb);
                }
            }
        }
    }
}

/*
 * Paint one row (or all of them), into the console's own surface.  A change
 * repaints its row and hands the UI just that rectangle, so a guest that
 * blinks a lamp twenty times a second does not repaint the whole panel.
 */
static void ipq_led_panel_paint(IPQLedPanel *s, int row)
{
    DisplaySurface *sf = qemu_console_surface(s->con);
    unsigned first, last, i;

    if (!sf) {
        return;
    }
    if (row < 0) {
        ipq_panel_fill(sf, 0, 0, s->w, s->h, IPQ_PANEL_BG);
        first = 0;
        last = s->n;
    } else {
        first = row;
        last = row + 1;
        ipq_panel_fill(sf, 0, IPQ_PANEL_PAD + row * IPQ_PANEL_ROW,
                       s->w, IPQ_PANEL_ROW, IPQ_PANEL_BG);
    }
    for (i = first; i < last && i < s->n; i++) {
        IPQLed *led = &s->leds[i];
        int y = IPQ_PANEL_PAD + i * IPQ_PANEL_ROW;

        ipq_panel_fill(sf, IPQ_PANEL_PAD, y + 3,
                       IPQ_PANEL_LAMP, IPQ_PANEL_LAMP, IPQ_PANEL_FRAME);
        ipq_panel_fill(sf, IPQ_PANEL_PAD + 2, y + 5,
                       IPQ_PANEL_LAMP - 4, IPQ_PANEL_LAMP - 4,
                       led->on ? IPQ_PANEL_ON : IPQ_PANEL_OFF);
        ipq_panel_text(sf, s->label_x, y + 2, led->name, IPQ_PANEL_TEXT);
    }
    dpy_gfx_update(s->con, 0, row < 0 ? 0 : IPQ_PANEL_PAD + row * IPQ_PANEL_ROW,
                   s->w, row < 0 ? s->h : IPQ_PANEL_ROW);
}

static void ipq_led_panel_refresh(IPQLedPanel *s, unsigned row)
{
    if (!s->con) {
        return;
    }
    ipq_led_panel_paint(s, (int)row);
}

static void ipq_led_panel_update(void *opaque)
{
    ipq_led_panel_paint(opaque, -1);
}

static void ipq_led_panel_invalidate(void *opaque)
{
    IPQLedPanel *s = opaque;

    dpy_gfx_update_full(s->con);
}

static const GraphicHwOps ipq_led_panel_ops = {
    .invalidate = ipq_led_panel_invalidate,
    .gfx_update = ipq_led_panel_update,
};

static void ipq_led_panel_realize(DeviceState *dev, Error **errp)
{
    IPQLedPanel *s = IPQ_LED_PANEL(dev);
    unsigned i;
    size_t maxlen = 4;

    for (i = 0; i < s->n; i++) {
        maxlen = MAX(maxlen, strlen(s->leds[i].name));
    }
    s->label_x = IPQ_PANEL_PAD + IPQ_PANEL_LAMP + 10;
    s->w = s->label_x + maxlen * IPQ_PANEL_ADVANCE + IPQ_PANEL_PAD;
    s->h = IPQ_PANEL_PAD * 2 + s->n * IPQ_PANEL_ROW;

    s->con = graphic_console_init(dev, 0, &ipq_led_panel_ops, s);
    qemu_console_resize(s->con, s->w, s->h);
    /* the first paint has to happen here: no UI may ever call gfx_update */
    ipq_led_panel_paint(s, -1);
}

static void ipq_led_panel_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipq_led_panel_realize;
    dc->desc = "ipq4019 board LED panel";
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ipq_led_panel_type = {
    .name = TYPE_IPQ_LED_PANEL,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IPQLedPanel),
    .class_init = ipq_led_panel_class_init,
};

/*
 * A pin was written: the lamp shows the level, if the level changed.  The
 * write happens on every access (gpiolib sets and clears a chip select around
 * each message), which is what makes an LED on an activity pin useful.
 */
static void ipq_led_line(void *opaque, int level)
{
    IPQLed *led = opaque;
    bool on = led->active_low ? level == 0 : level != 0;

    led->level = level;
    if (on == led->on) {
        return;
    }
    led->on = on;
    if (ipq_board->led_panel) {
        ipq_led_panel_refresh(IPQ_LED_PANEL(ipq_board->led_panel),
                              (unsigned)(led - ipq_board->leds));
    }
    if (env_true("QEMU_IPQ_LED_LOG")) {
        fprintf(stderr, "ipq4019: led %s -> %s\n", led->name,
                on ? "on" : "off");
    }
}

/* "al"/"low" and "ah"/"high" are the two polarities a DTS can name. */
static bool ipq_parse_polarity(const char *s, bool *low)
{
    if (!g_ascii_strcasecmp(s, "al") || !g_ascii_strcasecmp(s, "low") ||
        !strcmp(s, "1")) {
        *low = true;
        return true;
    }
    if (!g_ascii_strcasecmp(s, "ah") || !g_ascii_strcasecmp(s, "high") ||
        !strcmp(s, "0")) {
        *low = false;
        return true;
    }
    return false;
}

static uint32_t ipq_parse_gpio(const char *s, const char *what,
                               const char *item)
{
    uint64_t v;

    if (qemu_strtou64(s, NULL, 0, &v) < 0 || v >= IPQ_TLMM_NPINS) {
        error_report("ipq4019: %s=%s: '%s' is not a pin (0..%u)",
                     what, item, s, IPQ_TLMM_NPINS - 1);
        exit(1);
    }
    return (uint32_t)v;
}

/*
 * The pins are claimed when the part is bound, not when it is first used, so a
 * configuration that puts two parts on one pin is refused instead of the
 * second one silently taking the line.  A chip select is a part too.
 */
static void ipq_pin_claim(uint32_t gpio, const char *what, const char *item)
{
    if (gpio >= IPQ_TLMM_NPINS ||
        ipq->tlmm->handler[gpio] || ipq->tlmm->in_used[gpio]) {
        error_report("ipq4019: %s=%s: pin %u is already wired to something "
                     "else", what, item, gpio);
        exit(1);
    }
}

static void ipq_board_add_leds(void)
{
    gchar **items;
    unsigned n, i;

    if (!ipq_cfg.leds || !*ipq_cfg.leds) {
        return;
    }
    /* ';' separates items: -machine itself splits its arguments on ',' */
    items = g_strsplit_set(ipq_cfg.leds, ";,", -1);
    n = g_strv_length(items);
    ipq_board->leds = g_new0(IPQLed, n);

    for (i = 0; i < n; i++) {
        gchar **f;
        const char *item = g_strstrip(items[i]);
        IPQLed *led;
        bool low = false;

        if (!*item) {
            continue;
        }
        f = g_strsplit(item, ":", 3);
        if (!f[0] || !*f[0] || !f[1] || !*f[1]) {
            error_report("ipq4019: leds=%s: '%s' is not name:pin[:al]",
                         ipq_cfg.leds, item);
            exit(1);
        }
        if (f[2] && *f[2] && !ipq_parse_polarity(f[2], &low)) {
            error_report("ipq4019: leds=%s: '%s' is not al or ah",
                         ipq_cfg.leds, f[2]);
            exit(1);
        }

        led = &ipq_board->leds[ipq_board->n_leds];
        led->gpio = ipq_parse_gpio(f[1], "leds", item);
        ipq_pin_claim(led->gpio, "leds", item);
        led->name = g_strdup(f[0]);
        led->active_low = low;
        /* connect first: that is what latches the level the guest drives */
        ipq_tlmm_connect(ipq->tlmm, led->gpio, ipq_led_line, led);
        led->level = ipq_tlmm_out_level(ipq->tlmm, led->gpio);
        led->on = led->active_low ? led->level == 0 : led->level != 0;
        ipq_board->n_leds++;
        g_strfreev(f);
    }
    g_strfreev(items);

    if (!ipq_board->n_leds) {
        return;
    }

    {
        DeviceState *dev = qdev_new(TYPE_IPQ_LED_PANEL);
        IPQLedPanel *panel = IPQ_LED_PANEL(dev);

        panel->leds = ipq_board->leds;
        panel->n = ipq_board->n_leds;
        object_property_add_child(OBJECT(current_machine), "ipq-led-panel",
                                  OBJECT(dev));
        qdev_realize(dev, NULL, &error_fatal);
        ipq_board->led_panel = dev;
    }

    fprintf(stderr, "ipq4019: leds:");
    for (i = 0; i < ipq_board->n_leds; i++) {
        IPQLed *led = &ipq_board->leds[i];

        fprintf(stderr, " %s@%u%s", led->name, led->gpio,
                led->active_low ? "(al)" : "");
    }
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* the keys                                                            */
/* ------------------------------------------------------------------ */

typedef struct IPQKeyDevice {
    DeviceState parent;

    IPQKey *keys;               /* the board's array, not owned here */
    unsigned n;
} IPQKeyDevice;

#define TYPE_IPQ_GPIO_KEYS "ipq4019-gpio-keys"
OBJECT_DECLARE_SIMPLE_TYPE(IPQKeyDevice, IPQ_GPIO_KEYS)

/*
 * A key press changes the level a read of its pin reports.  The event stream
 * carries auto-repeat as repeated key-downs; the pin only cares about the edge,
 * so a second down on an already-held key is dropped here rather than waking
 * the guest twice.
 */
static void ipq_key_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    IPQKeyDevice *s = IPQ_GPIO_KEYS(dev);
    InputKeyEvent *key = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(key->key);
    unsigned i;

    if (qcode < 0) {
        return;
    }
    for (i = 0; i < s->n; i++) {
        IPQKey *k = &s->keys[i];
        int level;

        if (k->qcode != qcode || k->down == key->down) {
            continue;
        }
        k->down = key->down;
        level = k->active_low ? !k->down : k->down;
        ipq_tlmm_set_pin_in(ipq->tlmm, k->gpio, level);
        fprintf(stderr, "ipq4019: key %s %s (gpio %u, level %d)\n",
                k->name, k->down ? "pressed" : "released", k->gpio, level);
    }
}

static const QemuInputHandler ipq_key_handler = {
    .name = "ipq4019-gpio-keys",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = ipq_key_event,
};

static void ipq_keys_realize(DeviceState *dev, Error **errp)
{
    qemu_input_handler_register(dev, &ipq_key_handler);
}

static void ipq_keys_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipq_keys_realize;
    dc->desc = "ipq4019 board keys";
}

static const TypeInfo ipq_keys_type = {
    .name = TYPE_IPQ_GPIO_KEYS,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IPQKeyDevice),
    .class_init = ipq_keys_class_init,
};

/*
 * keys=name:pin[:al]:qcode
 *
 * The qcode is the name QMP's send-key uses (esc, f, f1, ...), so the same
 * spelling works from the window and from a headless test.  A key nobody
 * bound is not a button; a key bound to a pin the board already uses is an
 * error, exactly like an LED on a chip select.
 */
static void ipq_board_add_keys(void)
{
    gchar **items;
    unsigned n, i;

    if (!ipq_cfg.keys || !*ipq_cfg.keys) {
        return;
    }
    items = g_strsplit_set(ipq_cfg.keys, ";,", -1);
    n = g_strv_length(items);
    ipq_board->keys = g_new0(IPQKey, n);

    for (i = 0; i < n; i++) {
        gchar **f;
        const char *item = g_strstrip(items[i]);
        IPQKey *k;
        bool low = false;
        int nf, qcode;
        Error *err = NULL;

        if (!*item) {
            continue;
        }
        f = g_strsplit(item, ":", 4);
        nf = g_strv_length(f);
        if (nf < 3) {
            error_report("ipq4019: keys=%s: '%s' is not name:pin[:al]:key",
                         ipq_cfg.keys, item);
            exit(1);
        }
        if (nf == 4 && !ipq_parse_polarity(f[2], &low)) {
            error_report("ipq4019: keys=%s: '%s' is not al or ah",
                         ipq_cfg.keys, f[2]);
            exit(1);
        }
        qcode = qapi_enum_parse(&QKeyCode_lookup, f[nf - 1], -1, &err);
        if (qcode < 0) {
            error_report("ipq4019: keys=%s: '%s' is not a key name (%s)",
                         ipq_cfg.keys, f[nf - 1],
                         err ? error_get_pretty(err) : "no such key");
            error_free(err);
            exit(1);
        }

        k = &ipq_board->keys[ipq_board->n_keys];
        k->gpio = ipq_parse_gpio(f[1], "keys", item);
        ipq_pin_claim(k->gpio, "keys", item);
        k->name = g_strdup(f[0]);
        k->active_low = low;
        k->qcode = qcode;
        /* released: the level a read must report before anyone presses */
        ipq_tlmm_set_pin_in(ipq->tlmm, k->gpio, k->active_low ? 1 : 0);
        ipq_board->n_keys++;
        g_strfreev(f);
    }
    g_strfreev(items);

    if (!ipq_board->n_keys) {
        return;
    }

    {
        DeviceState *dev = qdev_new(TYPE_IPQ_GPIO_KEYS);
        IPQKeyDevice *kd = IPQ_GPIO_KEYS(dev);

        kd->keys = ipq_board->keys;
        kd->n = ipq_board->n_keys;
        object_property_add_child(OBJECT(current_machine), "ipq-gpio-keys",
                                  OBJECT(dev));
        qdev_realize(dev, NULL, &error_fatal);
        ipq_board->key_device = dev;
    }

    fprintf(stderr, "ipq4019: keys:");
    for (i = 0; i < ipq_board->n_keys; i++) {
        IPQKey *k = &ipq_board->keys[i];

        fprintf(stderr, " %s@%u%s->%s", k->name, k->gpio,
                k->active_low ? "(al)" : "", QKeyCode_str(k->qcode));
    }
    fprintf(stderr, "\n");
}

/* Both device types are registered from the machine itself: no meson change,
 * no new file, and the panel is only ever created when a board asked for it. */
static void ipq_board_ui_register_types(void)
{
    type_register_static(&ipq_led_panel_type);
    type_register_static(&ipq_keys_type);
}

type_init(ipq_board_ui_register_types)

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

struct IPQBam {
    MemoryRegion mr;
    uint32_t *shadow;           /* everything not explicitly modelled */
    uint8_t *valid;
    uint32_t nregs;
    uint32_t irq_srcs_msk;      /* BAM_IRQ_SRCS_MSK of EE0 */
    qemu_irq irq;               /* SPI 238, level high */
    IPQBamPipe pipe[BAM_PIPE_COUNT];
};

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

    if (!ipq->qup_spi) {
        return;
    }
    if (p->ctrl & BAM_CTRL_DIR_BAM2SYS) {
        for (i = 0; i < len; i++) {
            uint8_t v = qup_bam_rx(ipq->qup_spi);

            address_space_write(&address_space_memory, addr + i,
                                MEMTXATTRS_UNSPECIFIED, &v, 1);
        }
    } else {
        for (done = 0; done < len; done += i) {
            i = MIN(len - done, (uint32_t)sizeof(buf));
            address_space_read(&address_space_memory, addr + done,
                               MEMTXATTRS_UNSPECIFIED, buf, i);
            for (uint32_t j = 0; j < i; j++) {
                qup_bam_tx(ipq->qup_spi, buf[j]);
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
                        die_dbg_pc());
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
    IPQBam *b = ipq->blsp1_bam;
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
    IPQBam *b = ipq->blsp1_bam;
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
                        die_dbg_pc());
        }
        break;
    case BAM_P_FIFO_SIZES:
        b->pipe[n].ring_size = val & 0xffffu;
        if (die_dbg_on()) {
            die_dbg_log("die: bam sizes pipe=%d val=0x%08x pc=%08x\n",
                        n, (unsigned)val,
                        die_dbg_pc());
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
                        die_dbg_pc());
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
/* Ethernet DMA engine (IPQESS)                                        */
/* ------------------------------------------------------------------ */

/* Implemented with the MDIO/PHY model below; the packet path needs them first. */
static const char *ipq_port_name(int port);
static void ipq_eth_set_host_link(int switch_port, bool up);
static void ipq_eth_plug_host(int switch_port, bool up);

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

struct IPQEdma {
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
};


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
 * -netdev (default "net0") and eth-bridge-port the switch port wired to it
 * (default 5, the front-panel port the DTS calls lan1).  Without a matching
 * -netdev the machine still boots and simply drops every frame, the way it did
 * before there was a packet path at all.
 */
static void edma_connect_netdev(IPQEdma *e)
{
    const char *id = getenv("QEMU_IPQ_NETDEV");
    NetClientState *peer;
    NICState *nic;

    e->bridge_port = ipq_cfg.eth_bridge_port;
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
    ipq_eth_plug_host(e->bridge_port, e->host_link);
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
    ipq->edma = e;
    return e;
}

/* TX queue q is <GIC_SPI 0x41 + q>, RX queue q <GIC_SPI 0xf0 + q>. */
static void edma_wire_irqs(DeviceState *gicdev)
{
    unsigned q;

    if (!ipq->edma || !gicdev) {
        return;
    }
    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        ipq->edma->irq_tx[q] = qdev_get_gpio_in(gicdev, 0x41 + q);
        ipq->edma->irq_rx[q] = qdev_get_gpio_in(gicdev, 0xf0 + q);
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

struct IPQMdio {
    MemoryRegion mr;
    uint32_t mode;
    uint32_t addr;
    uint32_t wdata;
    uint32_t rdata;
    uint32_t cmd;
    int nphy;
    IPQPhy phy[IPQ_MDIO_MAX_PHYS];
};

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

    if (!ipq->mdio || switch_port < 1) {
        return;
    }
    p = mdio_lookup(ipq->mdio, switch_port - 1);
    if (!p) {
        return;
    }
    if (p->link != up) {
        info_report("ipq4019: %s: host link %s",
                    ipq_port_name(switch_port), up ? "up" : "down");
    }
    p->link = up;
}

/*
 * The host's wire goes into one socket.  A board in front of someone has that
 * one cable in it, so attaching a backend is also saying what the rest of the
 * front panel looks like: the port the -netdev sits behind gets the host's
 * carrier, and every other copper PHY is an empty socket that reads link down.
 * Only done when a backend exists -- with no -netdev there is no host wire at
 * all, and the board's own eth-ports link states are what the guest sees.
 */
static void ipq_eth_plug_host(int switch_port, bool up)
{
    int i;

    if (!ipq->mdio) {
        return;
    }
    for (i = 0; i < ipq->mdio->nphy; i++) {
        IPQPhy *p = &ipq->mdio->phy[i];

        /* the port with the wire, and the switch's PCS (not a socket) */
        if (p->addr == switch_port - 1 || p->addr == IPQ_PSGMII_ADDR) {
            continue;
        }
        p->link = 0;
    }
    ipq_eth_set_host_link(switch_port, up);
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
 * Parse "addr[:speed[:id]]" entries separated by ';' (or ',' for the old
 * environment spelling).  A leading '!' on the port means "link down"; that is
 * how a board with an empty SFP cage or a switch port with nothing plugged in
 * looks.  An entry that does not name an ID gets the board's PHY identifier.
 */
static void mdio_parse_ports(IPQMdio *m, const char *spec, uint32_t def_id)
{
    while (spec && *spec) {
        char *end;
        long addr, speed = 1000, link = 1;
        uint32_t id = def_id;

        while (*spec == ',' || *spec == ';' || *spec == ' ') {
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
            warn_report("ipq4019: bad Ethernet port entry at '%s'", spec);
            break;
        }
        spec = end;
        /* '!' next to the address -- "!5", "5!", "5:!100" -- is "no cable" */
        while (*spec == '!') {
            link = 0;
            spec++;
        }
        if (*spec == ':' && spec[1] == '!') {
            link = 0;
            spec++;
        }
        if (*spec == ':') {
            speed = strtol(spec + 1, &end, 0);
            spec = end;
        }
        if (*spec == '!') {
            link = 0;
            spec++;
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
    int i;

    /*
     * Three sources, in order of precedence: the machine's port list, the
     * board device tree, the built-in QCA8075 population.  The port list is
     * board data in the same sense as the dies: a board with two PHYs is not a
     * reference design with four missing, it is a different board.
     */
    if (ipq_cfg.eth_ports) {
        /* An empty list is a board with no switch on the bus, not "unspecified":
         * someone who wrote eth-ports= has said what is there.  The resolved
         * configuration is non-NULL only when the board named its PHYs --
         * with the property, or through its preset -- so this also covers a
         * board preset whose population differs from the reference design. */
        if (*ipq_cfg.eth_ports) {
            mdio_parse_ports(m, ipq_cfg.eth_ports, ipq_cfg.eth_phy_id);
        }
    } else if (dtb_path) {
        mdio_add_from_dtb(m, dtb_path);
        if (!m->nphy) {
            mdio_parse_ports(m, IPQ_ETH_DEFAULT_PORTS, ipq_cfg.eth_phy_id);
        }
    } else {
        mdio_parse_ports(m, IPQ_ETH_DEFAULT_PORTS, ipq_cfg.eth_phy_id);
    }

    /*
     * The switch's PSGMII control block is not a front-panel PHY but it is on
     * the same bus, at address 5.  It has to answer even when the board named
     * only its copper PHYs: RouterOS's device tree leaves it out because its
     * driver never addresses it, while OpenWrt's tree hangs `psgmii-phy@5`
     * off the mdio node and qca8k refuses to bind when that read fails
     * ("unable to get PSGMII eth PHY") -- no DSA ports, no bridge, nothing.
     */
    if (m->nphy && !mdio_lookup(m, IPQ_PSGMII_ADDR)) {
        mdio_add_phy(m, IPQ_PSGMII_ADDR, 1000, ipq_cfg.eth_phy_id, 1);
    }

    ipq->mdio = m;
    memory_region_init_io(&m->mr, NULL, &mdio_ops, m, "mdio", IPQ_MDIO_SIZE);
    memory_region_add_subregion(parent, IPQ_MDIO_BASE, &m->mr);

    if (!m->nphy) {
        fprintf(stderr, "ipq4019: mdio@%08x, no port\n", IPQ_MDIO_BASE);
    } else {
        fprintf(stderr, "ipq4019: mdio@%08x, %d port(s):", IPQ_MDIO_BASE,
                m->nphy);
        for (i = 0; i < m->nphy; i++) {
            fprintf(stderr, " %d@%d%s", m->phy[i].addr, m->phy[i].speed,
                    m->phy[i].link ? "" : "(down)");
        }
        fprintf(stderr, "\n");
    }
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
struct IPQUart {
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
    /*
     * Set the first time software turns on the BAM receiver on this port, and
     * cleared only by a device reset.  This is what tells a PIO reader (the
     * bootloaders, which poll MISR bit 3 and take bytes out of the FIFO word)
     * from a DMA reader (msm_serial), and it has to be remembered because the
     * kernel drops RX_BAM_ENABLE in the very read that asks for the snapshot.
     */
    bool rx_dma_seen;
    bool rx_stale;              /* partial word waiting for the timeout */
    QEMUTimer *stale_timer;
};

/* The console UART, so the BAM UART TX pipe can drive the same backend. */

/* Set when the machine instantiates the GIC, so devices added later can
 * claim their SPI input line. */

#define UART_RX_FIFO_SIZE   (sizeof(((IPQUart *)0)->rxbuf))

/* Stale (partial word) timeout, in microseconds of virtual time. */
#define UART_RX_STALE_US    1000

/*
 * UARTDM_DMEN bit 3: the receiver is drained by the BAM instead of the
 * FIFO/watermark interrupt path (UARTDM v1.4).
 */
#define UART_DMEN_RX_BAM_ENABLE (1u << 3)

/*
 * Is the receiver in PIO mode?
 *
 * The two halves of the receiver want different things from the same
 * registers, and the bootloaders use the other half than the kernel does:
 *
 *   PIO (DMEN RX BAM clear)  SBL1, RouterBOOT and U-Boot poll MISR bit 3 and
 *                            then take bytes straight out of the FIFO word.
 *                            Nothing else ever looks at the FIFO, so bit 3
 *                            has to say "a byte is buffered" and RX_TOTAL_SNAP
 *                            has to be the FIFO depth.
 *
 *   BAM (DMEN RX BAM set)     msm_serial never touches the FIFO: it posts a
 *                            descriptor, the engine moves bytes into DRAM and
 *                            the idle timeout retires the buffer.  Its
 *                            interrupt handler services receive on `tst
 *                            r6,#0x18`, so bit 3 is already part of what it
 *                            accepts, and RX_TOTAL_SNAP is the bytes moved to
 *                            DRAM since the buffer was posted.
 *
 * Every rule below therefore keys off this: a PIO reader gets the bootloader
 * contract, a DMA reader keeps the behaviour it has always had.
 */
static bool uart_rx_is_pio(const IPQUart *u)
{
    /*
     * Latched, not `u->dmen`: msm_serial clears RX_BAM_ENABLE around its own
     * housekeeping and then still reads RX_TOTAL_SNAP to learn how many bytes
     * the engine moved (measured: DMEN clear, one byte moved, so a live test
     * reports the FIFO's 0 and the driver throws the character away).  Once a
     * BAM receiver has been armed on this port the reader is that kind of
     * reader for good; a bootloader never arms it at all.
     */
    return !u->rx_dma_seen;
}

static uint32_t uart_isr(IPQUart *u)
{
    uint32_t isr = 0;

    /* The TX FIFO is always drained by the time software looks at it. */
    isr |= 0x01;                    /* TXLEV */
    if (u->rx_count >= 4) {
        isr |= 0x10;                /* RXLEV */
    }
    /*
     * Bit 3 is "receive data available" -- the same bit the kernel's handler
     * accepts as part of `tst r6,#0x18`.  RouterBOOT's getc() (0x81001e48)
     * and U-Boot's tstc (0x87334b34) test this one bit and nothing else
     * before taking a character, so a PIO reader that sees it clear never
     * reads the FIFO and a key press does nothing.  RXSTALE keeps raising it
     * as well, which is what it meant before.
     */
    if (u->rx_stale || (uart_rx_is_pio(u) && u->rx_count)) {
        isr |= 0x08;                /* RX_READY / RXSTALE */
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
    uart_rx_fill(ipq->console_uart);
}

/* Run the idle timeout that retires a partially filled receive descriptor. */
static void uart_rx_idle_arm(void)
{
    IPQUart *u = ipq->console_uart;

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
    return ipq->console_uart &&
           (ipq->console_uart->dmen & UART_DMEN_RX_BAM_ENABLE);
}

static size_t uart_bam_rx_pending(void)
{
    return ipq->console_uart ? ipq->console_uart->rx_count : 0;
}

static size_t uart_bam_rx_pull(uint8_t *buf, size_t n)
{
    IPQUart *u = ipq->console_uart;
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
    if (ipq->console_uart) {
        ipq->console_uart->rx_total += n;
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
         off == 0x70 || off == 0x100)) {
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
         * Cumulative byte count since the last snapshot, cleared by the
         * read: msm_serial_dm() takes `snap - old_snap_state` (what is still
         * queued) and then zeroes old_snap_state, so the counter has to
         * restart from zero here or the next stale event would report the
         * bytes that were already handed to the tty.
         *
         * A PIO reader wants something else from the same register: the
         * number of bytes it is about to take out of the FIFO word.
         * RouterBOOT's getc() (0x81001e84) reads this once, keeps it as its
         * own byte counter and takes min(snap,4) bytes out of the word, so a
         * constant 0 turns every key press into a zero-length character that
         * is silently discarded.
         */
        uint32_t snap = uart_rx_is_pio(u) ? u->rx_count : u->rx_total;

        UART_RX_LOG("%s SNAP -> %u\n", u->name, snap);
        if (!uart_rx_is_pio(u)) {
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
            state_note_tx(buf, n);
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
        } else if ((val >> 4 & 0xf) == 8) { /* RESET_STALE_INT */
            u->rx_stale = false;
            timer_del(u->stale_timer);
        } else if (uart_rx_is_pio(u) && (val >> 4 & 0xf) == 3) {
            /*
             * RESET_ERROR_STATUS.  RouterBOOT's getc() writes this and
             * re-initialises the port when it reads a stale receive word, so
             * the error has to clear here or it would retry against the same
             * state forever.
             *
             * PIO only, deliberately: in DMA mode this timer is what retires
             * a partially filled receive descriptor, and cancelling it would
             * strand the driver's posted buffer.
             */
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
        if (val & UART_DMEN_RX_BAM_ENABLE) {
            u->rx_dma_seen = true;
            /*
             * Arming the receiver starts feeding the posted descriptor.  The
             * driver clears RX_BAM_ENABLE for its own housekeeping, so bytes
             * can arrive while the engine is off; the stale timeout they
             * would otherwise wait for has already been consumed by the PIO
             * branch of uart_stale_expire (which sets RXSTALE and does not
             * rearm, because a DMA reader has no FIFO interrupt).  Nothing
             * would ever move them: the guest stops reading in the middle of
             * a command line and the console is dead from then on.
             */
            bam_uart_rx_kick();
            if (u->rx_count) {
                uart_rx_idle_arm();
            }
        }
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
    if (!ipq->console_uart) {
        ipq->console_uart = u;
    }
}

/* Bytes handed to the console UART by its BAM TX pipe (DMA console output). */
static void uart_bam_write(const uint8_t *buf, size_t n)
{
    size_t i;

    if (!ipq->console_uart) {
        return;
    }
    for (i = 0; i < n; i++) {
        die_dbg_ser(buf[i]);
    }
    qemu_chr_fe_write_all(&ipq->console_uart->chr, buf, n);
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

static void ipq_state_dump(const char *tag)
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
static void state_note_tx(const uint8_t *buf, unsigned n)
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
    {
        IPQPsHold *ps = g_new0(IPQPsHold, 1);
        MemoryRegion *mr = g_new0(MemoryRegion, 1);

        ps->val = 1;                    /* rails held up out of reset */
        ipq->pshold = ps;
        memory_region_init_io(mr, NULL, &pshold_ops, ps, "pshold", 0x1000);
        memory_region_add_subregion(sysmem, IPQ_PSHOLD_BASE, mr);
    }
    {
        IPQWdt *w = g_new0(IPQWdt, 1);

        w->bark = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_bark_expire, w);
        w->bite = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_bite_expire, w);
        ipq->wdt = w;
        memory_region_init_io(&w->mr, NULL, &wdt_ops, w, "wdt", 0x1000);
        /* The page sits inside the 0x0b000000 apcs stub. */
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
    add_tlmm(sysmem, IPQ_TLMM_BASE, IPQ_TLMM_SIZE, "tlmm");
    /*
     * The board says which pins drive the dies' chip selects, so that is where
     * the dies are connected: the pin controller is the SoC's, the wiring
     * between it and the parts is the board's.
     */
    if (ipq_cfg.nor) {
        ipq_tlmm_connect(ipq->tlmm, ipq_cfg.nor_cs_gpio, ipq_cs_line,
                         (void *)(uintptr_t)NAND_CS_NOR);
    }
    if (ipq_cfg.nand) {
        ipq_tlmm_connect(ipq->tlmm, ipq_cfg.nand_cs_gpio, ipq_cs_line,
                         (void *)(uintptr_t)NAND_CS_NAND);
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
    add_stub_ex(sysmem, 0x40000000, 0x00010000, "pcie0-dbi", 0);
    add_stub_ex(sysmem, 0x40100000, 0x00001000, "pcie0-config", 0);

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
        "board preset: default, none, mikrotik-952 (or a command-line "
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
