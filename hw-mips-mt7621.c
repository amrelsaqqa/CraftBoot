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
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "hw/boards.h"
#include "hw/char/serial-mm.h"
#include "hw/char/serial.h"
#include "hw/intc/mips_gic.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/mips/mips.h"
#include "hw/qdev-clock.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "net/net.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "qemu/log.h"
#include "exec/icache-shadow.h"
#include "hw/timer/mips_gictimer.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/block-backend.h"
#include "target/mips/cpu-qom.h"
#include "target/mips/internal.h"

#define TYPE_MT7621_MACHINE MACHINE_TYPE_NAME("mt7621")
OBJECT_DECLARE_SIMPLE_TYPE(MT7621State, MT7621_MACHINE)

typedef enum MT7621Board {
    MT7621_BOARD_AUTO = 0,
    MT7621_BOARD_LG_GAPM7200,     /* 256 MiB DDR, parallel NAND */
    MT7621_BOARD_RB750GR3,        /* 256 MiB DDR, 16 MiB SPI NOR */
} MT7621Board;

/* SoC memory map */
#define MT7621_RAM_BASE         0x00000000
#define MT7621_PERIPH_BASE      0x1e000000
#define MT7621_PERIPH_SIZE      0x00100000

#define MT7621_SYSCTL_OFF       0x00000000
/*
 * SPI NOR is memory-mapped by the BootROM at 0x1fc00000.  The MPU decodes
 * a 4 MiB window there, which is all the boot chain needs: RouterBOOT's
 * second stage lives at flash offset 0x10000.
 */
#define MT7621_SPI_FLASH_BASE   0x1fc00000
#define MT7621_SPI_WINDOW       0x00400000
#define MT7621_SYSTICK_OFF      0x00000500
#define MT7621_UART0_OFF        0x00000c00
#define MT7621_NFI_OFF          0x00003000

/* MT7621 NAND flash interface (register map matches Linux mtk_nand) */
#define NFI_CNFG                0x00
#define NFI_PAGEFMT             0x04
#define NFI_CON                 0x08
#define NFI_ACCCON              0x0c
#define NFI_INTR_EN             0x10
#define NFI_INTR_STA            0x14
#define NFI_CMD                 0x20
#define NFI_ADDRNOB             0x30
#define NFI_COLADDR             0x34
#define NFI_ROWADDR             0x38
#define NFI_STRDATA             0x40
#define NFI_CNRNB               0x44
#define NFI_DATAW               0x50
#define NFI_DATAR               0x54
#define NFI_PIO_DIRDY           0x58
#define NFI_STA                 0x60
#define NFI_ADDRCNTR            0x70
#define NFI_CSEL                0x90

#define MT7621_NAND_PAGE_SIZE   2048
#define MT7621_NAND_OOB_SIZE    64
#define MT7621_NAND_PAGES_BLK   64
#define MT7621_DRAMC_OFF        0x00005000

/*
 * Interrupt wiring taken from the DTB appended to the flash kernel:
 * a MIPS GIC at 0x1fbc0000, UART0 on shared interrupt 26 and the
 * "ralink,mt7621-systick" 50 kHz counter on shared interrupt 5.
 */
#define MT7621_GIC_BASE         0x1fbc0000
/*
 * 64 shared interrupts, not an arbitrary 256.  RouterBOOT validates the
 * GIC before it will use it: it reads GIC_SH_CONFIG and requires the
 * NUMINTRS field (bits 23:16, which QEMU derives as num_irq / 8 - 1) to be
 * exactly 7 -- i.e. 64 interrupts.  Our own devices only use IRQ 5 and 26,
 * and Linux sizes the GIC from this same field, so 64 is both the hardware
 * truth and strictly safer.
 */
#define MT7621_GIC_NUM_IRQ      64
#define MT7621_UART0_IRQ        26
#define MT7621_SYSTICK_IRQ      5

/*
 * ralink,cevt-systick: a 16-bit up-counter clocked at 50 kHz.  Linux
 * programs COMPARE with "COUNT + delta" and expects an interrupt when
 * the counter reaches it.
 */
#define MT7621_SYSTICK_HZ       50000
#define MT7621_SYSTICK_PERIOD   50      /* 1 ms at 50 kHz */
#define SYSTICK_CONFIG          0x00
#define SYSTICK_COMPARE         0x04
#define SYSTICK_COUNT           0x08
#define SYSTICK_CFG_CNT_EN      (1u << 0)
#define SYSTICK_CFG_EXT_STK_EN  (1u << 1)
/* Every KSEG0/KSEG1 physical address the CPU can generate, for aliasing. */
#define MT7621_KSEG1_WINDOW     0x20000000
/*
 * Frame engine / switch register block.  The DT says 64 KiB but the MDIO
 * control register lives at 0x10004, so map 128 KiB.
 */
#define MT7621_ETH_SIZE         0x00020000
#define MT7621_ETH_IAC          0x10004

#define ETH_IAC_ACCESS          (1u << 31)
#define ETH_IAC_WRITE           (1u << 18)
#define ETH_IAC_START           (1u << 16)
#define ETH_IAC_ADDR_SHIFT      20
#define ETH_IAC_REG_SHIFT       25

/*
 * PDMA ring block at 0x1e100800 (MediaTek SDK layout, also what RouterBOOT
 * programs):
 *   0x0800 TX base ptr   0x0804 TX max cnt   0x0808 TX ctx idx  0x080c TX dtx idx
 *   0x0900 RX base ptr   0x0904 RX max cnt   0x0908 RX crx idx  0x090c RX drx idx
 *   0x0a04 PDMA global config (TX_EN bit0, RX_EN bit2)
 * Linux only uses the receive half here and transmits through QDMA (0x1a00),
 * but the bootloader queues its DHCP frames on this TX ring.
 */
#define ETH_PTX_BASE_PTR0       0x0800
#define ETH_PTX_MAX_CNT0        0x0804
#define ETH_PTX_CTX_IDX0        0x0808
#define ETH_PTX_DTX_IDX0        0x080c
#define ETH_PDMA_GLO_CFG        0x0a04

/* PDMA TX descriptor (16 bytes): {SDP0, len/LS0/DDONE, SDP1, fport/...} */
#define PTX_DDONE               (1u << 31)
#define PTX_LS0                 (1u << 30)
#define PTX_SDL0(w)             (((w) >> 16) & 0x3fff)
#define PTX_LS1                 (1u << 14)
#define PTX_SDL1(w)             ((w) & 0x3fff)
#define PTX_FPORT(w)            (((w) >> 25) & 0x7)

/* DMA engines (MT7621: PDMA receive rings, QDMA transmit) */
#define ETH_PRX_BASE_PTR0       0x0900
#define ETH_PRX_MAX_CNT0        0x0904
#define ETH_PRX_CRX_IDX0        0x0908
#define ETH_PRX_DRX_IDX0        0x090c
#define ETH_PDMA_INT_STATUS     0x0a20
#define ETH_PDMA_INT_MASK       0x0a28
#define ETH_QDMA_GLO_CFG        0x1a04
#define ETH_QTX_CTX_PTR         0x1b00
#define ETH_QTX_DTX_PTR         0x1b04
#define ETH_QTX_DRX_PTR         0x1b14
#define ETH_QDMA_INT_STATUS     0x1a18
#define ETH_QDMA_INT_MASK       0x1a1c

#define PDMA_RX_DONE            (1u << 30)
/*
 * The Linux raeth driver masks 0x0003000f in PDMA_INT_MASK: TX done for the
 * four queues in bits 0..3 and RX done for the two RX rings in bits 16..17.
 * RouterBOOT only polls, so these used to be missing entirely - which left
 * Linux waiting for TX completions until NETDEV WATCHDOG fired.
 */
#define PDMA_TX_DONE0           (1u << 0)
/*
 * Note: bit 16 (RX_DONE_INT0) is deliberately left alone.  The driver polls
 * its RX index, and raising the interrupt so early makes net_rx_action()
 * run before the switch driver has registered its NAPI instances, so
 * n->poll is NULL and the kernel faults with epc=0.
 */
#define QDMA_TX_DONE            (1u << 28)
/*
 * TX descriptor ownership: the CPU fills a descriptor (owner bit clear) and
 * the DMA engine sets it again once the frame has left the chip.  The driver
 * only reclaims descriptors whose owner bit is set, so it must be written
 * back here.
 */
#define TX_DMA_OWNER_CPU        (1u << 31)
#define TX_DMA_FPORT_SHIFT      25
#define TX_DMA_TSO              (1u << 28)
#define TX_DMA_LS0              (1u << 30)   /* last buffer of this frame */
#define TX_DMA_CHKSUM           (0x7u << 29)
/* QDMA TX ring: 2048 descriptors of 16 bytes. */
#define TX_RING_BYTES           0x8000

/* RX descriptor bits. */
#define RX_DMA_FPORT_SHIFT      19
#define RX_DMA_SPECIAL_TAG      (1u << 22)
#define TX_DMA_PLEN0(x)         ((((x) & 0x3fff)) << 16)
#define RX_DMA_DONE             (1u << 31)
#define RX_DMA_PLEN0(x)         ((((x) & 0x3fff)) << 16)

/*
 * MIPS Coherence Manager (CM) / Cluster Power Controller (CPC).
 * CPC sits at 0x1fbf0000, the global control registers (GCR) at 0x1fbf8000.
 */
#define MT7621_CM_BASE          0x1fbf0000
#define MT7621_CM_SIZE          0x00010000
#define MT7621_MAX_CPUS         4
#define MT7621_GCR_OFF          0x00008000

/* GCR register offsets */
#define GCR_CONFIG              0x0000
#define GCR_BASE                0x0008
#define GCR_REV                 0x0030
#define GCR_GIC_BASE            0x0080
#define GCR_CPC_BASE            0x0088
#define GCR_GIC_STATUS          0x00d0
#define GCR_CPC_STATUS          0x00f0
#define GCR_CL_OTHER            0x2018
#define GCR_CL_ID               0x2028
#define GCR_CO_RESET_RELEASE    0x4000

/* CPC register offsets (relative to the CPC base) */
#define CPC_CL_OTHER            0x0010
#define CPC_CL_BASE             0x2010
#define CPC_CO_CMD              0x4000

#define MT7621_UART_CLOCK       50000000
#define MT7621_UART_BAUD_BASE   (MT7621_UART_CLOCK / 16)

/* sysctl register offsets we care about */
#define SYS_CHIP_NAME0          0x00
#define SYS_CHIP_NAME1          0x04
#define SYS_CHIP_REV_ID         0x0c
#define SYS_SYSCFG0             0x10
#define MT7621_CHIP_MODE        1       /* SYSCFG0[3:0]: parallel NAND */
#define SYS_CLKCFG0             0x2c
#define SYS_RSTCTL              0x34
#define SYS_RSTSTAT             0x38
#define WDT_RST_CAUSE           (1u << 1)

/*
 * Ralink timer/watchdog block in the system controller's second 256 byte
 * page (1e000100).  Linux's mt7621_wdt drives timer 1: TMR1LOAD holds the
 * reload value in prescaler ticks, TMR1CTL bit 7 enables counting and bits
 * 16..31 are the prescaler in microseconds.  Writing TMRSTAT bit 9 reloads
 * (that is the "ping"); letting it run out resets the SoC.
 */
#define MT7621_WDT_OFF          0x100
#define MT7621_WDT_SIZE         0x100
#define WDT_TMRSTAT             0x00
#define WDT_TMR1CTL             0x20
#define WDT_TMR1LOAD            0x24
#define WDT_TMRSTAT_RESTART     (1u << 9)
#define WDT_TMR1CTL_ENABLE      (1u << 7)
#define SYS_CUR_CLK_STS         0x44
#define SYS_GPIO_MODE           0x60

#define SYSCTL_WORDS            (0x1000 / 4)
#define DRAMC_WORDS             (0x1000 / 4)
#define NFI_WORDS               (0x1000 / 4)

struct MT7621State {
    MachineState parent;

    MIPSCPU *cpu;               /* CPU0: keeps the existing tracing paths */
    MIPSCPU *cpus[MT7621_MAX_CPUS];
    unsigned ncpus;
    Clock *cpuclk;
    /* Latched by mt7621_cm_write() so a CPC reset can start that core. */
    uint32_t co_other;          /* GCR_CL_OTHER: core selector (CM < 3) */
    uint32_t cpc_other;         /* CPC_CL_OTHER: core selector for the CPC */
    uint32_t co_reset_base;
    DeviceState *gic;

    MemoryRegion ram;
    MemoryRegion periph;
    MemoryRegion sysctl;
    MemoryRegion rstctl;
    MemoryRegion wdt;
    MemoryRegion systick;
    MemoryRegion dramc;
    MemoryRegion nfi;
    MemoryRegion sram;            /* unused, kept for layout stability */
    uint32_t eth_regs[MT7621_ETH_SIZE / 4];
    /* MT7530 indirect register file (page/lo/hi via MDIO addr 0x1f) */
    uint32_t sw_regs[0x400 * 16];
    uint16_t sw_page;
    uint16_t sw_r;

    NICState *nic;
    NICConf nic_conf;
    MemReentrancyGuard nic_guard;
    qemu_irq eth_irq;
    uint32_t rx_drx_idx;
    uint32_t ptx_idx;           /* PDMA transmit ring consumer index */
    int64_t phy_link_ns;        /* when the tapped PHY reports link up */
    uint16_t phy_regs[32][32];  /* MDIO register file per address */

    /* optional writable NAND image (-drive if=none,id=nand) */
    BlockBackend *nand_blk;
    int nand_fd;
    bool nand_wr_error;
    QEMUTimer *wdt_timer;
    bool wdt_rst_cause;
    unsigned eth_port;          /* DSA user port exposed to the backend */
    uint32_t qtx_dtx;           /* forward DMA pointer (next to send) */
    uint32_t qtx_base;          /* start of the TX descriptor ring */
    bool qtx_en;                /* QDMA TX DMA enable */
    MemoryRegion cm;
    MemoryRegion eth;
    MemoryRegion fe_sram;       /* 0x1e108000: FE/PSE SRAM used before DDR */
    MemoryRegion ram_alias[8];
    int ram_alias_count;

    uint32_t sysctl_regs[SYSCTL_WORDS];
    uint32_t systick_regs[4];
    uint32_t dramc_regs[DRAMC_WORDS];
    uint32_t nfi_regs[NFI_WORDS];

    /* NAND flash contents and the current PIO transfer */
    uint8_t *nand;
    uint64_t nand_size;
    uint32_t nand_stride;       /* bytes per page in the dump (2048/2112) */
    uint32_t nand_pages;        /* pages in the dump */
    uint8_t nfi_fifo[MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE];
    uint32_t nfi_fifo_len;
    uint32_t nfi_fifo_pos;
    uint32_t nfi_col;
    uint32_t nfi_row;
    uint32_t nfi_served;
    uint32_t nfi_last_cmd;
    /* Address bytes the kernel driver sends one at a time via COLADDR. */
    uint8_t nfi_addr[8];
    uint32_t nfi_addr_len;
    uint32_t nfi_addr_pending;

    /* page program buffer */
    uint8_t nfi_wbuf[MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE];
    uint32_t nfi_wbuf_len;
    bool nfi_writing;
    uint32_t cm_regs[MT7621_CM_SIZE / 4];

    /* systick state: count = base + elapsed, anchored at base_ns */
    QEMUTimer *poweroff_timer;  /* detects a parked CPU (poweroff/halt) */
    unsigned halt_ticks;
    bool halt_seen_running;

    /*
     * Kernel command line injection for bring-up.  RouterBOOT assembles its
     * command line in a scratch buffer and hands the pointer to the kernel;
     * MT7621_CMDLINE appends options to that buffer before the kernel gets
     * to parse it (see mt7621_cmdline_poll()).
     */
    QEMUTimer *cmdline_timer;
    QEMUTimer *flash_save_timer;   /* MT7621_FLASH_SAVE: periodic flash dump */
    char *cmdline_extra;
    uint32_t cmdline_cursor;
    uint32_t cmdline_phys;      /* discovered buffer address, 0 = unknown */
    unsigned cmdline_ticks;
    bool cmdline_kernel_seen;
    unsigned cmdline_patches;
    int64_t cmdline_deadline_ns;

    QEMUTimer *systick_timer;
    QEMUTimer *systick_pulse_off;
    qemu_irq systick_irq;
    int64_t systick_base_ns;
    uint32_t systick_base_count;
    bool systick_pending;

    /* board preset */
    char *board_name;
    MT7621Board board;
    uint32_t chip_mode;         /* SYSCFG0[3:0] boot flash select */

    /* SPI NOR image mapped at 0x1fc00000 */
    uint8_t *flash;
    uint64_t flash_size;
    MemoryRegion flash_mr;
    bool spi_nor;
    bool elf_kernel;            /* -kernel is an ELF to load, not a dump */
    uint32_t elf_argc;
    uint32_t elf_argv;
    bool entry_logged;
    bool rom_logged;
#define PC_HIST 256
    uint32_t pc_hist[PC_HIST];
    unsigned pc_hist_idx;

    /*
     * SPI controller at 0x1e000b00.  The MT7621 maps the NOR chip into a
     * 4 MiB window at 0x1fc00000 and also exposes the controller's PIO
     * FIFO; RouterBOOT and Linux both use the latter to read the rest of
     * the chip.
     */
    MemoryRegion spi;
    uint32_t spi_regs[0x100 / 4];
    uint8_t spi_rx[32];
    bool spi_read_mode;
    uint32_t spi_addr;
    /*
     * Winbond RDUID (0x4B) value.  RouterOS derives its software-id (and
     * therefore validates its license key) from this, so it has to be
     * stable and configurable per board: -M mt7621,spi-uid=0x...
     */
    uint64_t spi_uid;

    /* boot image */
    uint32_t load_addr;
    uint32_t entry;
    bool uimage;
    char *image_file;           /* boot image, reloaded on reset */

    /*
     * Bring-up knob: L1 cache geometry reported in Config1 bits 15:0
     * (IS/IL/IA/DS/DL/DA).  Linux picks its cache flush strategy from
     * these fields, so a value that lies about the geometry can make the
     * guest call a flush routine pointer that was never installed.
     */
    uint32_t cfg1_cache_fields;
};

static bool mt7621_trace;

/*
 * Boolean env knob.  These ML-style knobs used to be tested with a bare
 * getenv(), which is true for *any* value -- including "0" -- so a launcher
 * that always exports MT7621_FOO=0 silently turned the feature on.  Treat
 * the usual false spellings as off.
 */
static bool mt7621_flag(const char *name)
{
    const char *v = getenv(name);

    if (!v || !*v) {
        return false;
    }
    return strcmp(v, "0") && strcmp(v, "no") && strcmp(v, "off") &&
           strcmp(v, "false");
}


/*
 * The GPIO block lives inside the system controller.  Bits set in the
 * direction register drive the pin, every other bit is an input.  The board
 * wires its buttons active-low with pull-ups, so an *idle* input reads 1;
 * echoing the output latch back for input bits makes an unconnected pin look
 * permanently pressed (Breed then aborts autoboot with "button press").
 */
#define SYS_GPIO_CTRL           0x600
#define SYS_GPIO_DATA           0x620
#define SYS_GPIO_DSET           0x634
#define SYS_GPIO_DCLR           0x638
#define SYS_GPIO_MASK           0x00ffffffu

static uint32_t mt7621_gpio_inputs(uint32_t *regs)
{
    uint32_t dir = regs[SYS_GPIO_CTRL / 4] & SYS_GPIO_MASK;
    uint32_t out = regs[SYS_GPIO_DATA / 4] & dir;

    /* Input bits read their pulled-up idle level. */
    return out | (~dir & SYS_GPIO_MASK);
}

static uint64_t mt7621_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t off = addr & 0xfff;
    uint32_t val = regs[off >> 2];

    if (off == SYS_GPIO_DATA) {
        val = mt7621_gpio_inputs(regs);
    }

    /*
     * The block is 32-bit oriented, but the SoC happily answers byte and
     * halfword loads (MikroTik's 3.3.5 kernel reads the chip revision with
     * an lhu).  Shifting here keeps those from turning into a bus error.
     */
    if (size < 4) {
        val >>= 8 * (off & 3);
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: reg read  %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    return val;
}

static void mt7621_reg_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    uint32_t *regs = opaque;
    uint32_t off = addr & 0xfff;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: reg write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }

    if (size < 4) {
        /* Merge the written lane into the current register value. */
        unsigned shift = 8 * (off & 3);
        uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << shift;

        val = (regs[off >> 2] & ~mask) | (((uint32_t)val << shift) & mask);
        regs[off >> 2] = val;
        return;
    }

    switch (off) {
    case SYS_GPIO_DATA:
    {
        /* Only pins configured as outputs latch a value. */
        uint32_t dir = regs[SYS_GPIO_CTRL / 4] & SYS_GPIO_MASK;

        regs[SYS_GPIO_DATA / 4] =
            (regs[SYS_GPIO_DATA / 4] & ~dir) | ((uint32_t)val & dir);
        return;
    }
    case SYS_GPIO_DSET:
        regs[SYS_GPIO_DATA / 4] |= (uint32_t)val;
        return;
    case SYS_GPIO_DCLR:
        regs[SYS_GPIO_DATA / 4] &= ~(uint32_t)val;
        return;
    default:
        break;
    }

    regs[off >> 2] = val;
}

/*
 * Ethernet frame engine + MDIO.  Only what firmware needs to identify the
 * PHY and bring the MAC up is modelled: the MDIO control register performs
 * the transaction immediately, and the switch/PHY at address 31 answers
 * with a plausible MT7530 identity.
 */
static void mt7621_eth_update_irq(MT7621State *s);
static void mt7621_eth_tx(MT7621State *s, uint32_t ctx);
/*
 * MT7621 switch special tag
 * ------------------------
 * The switch driver inserts four bytes after the source MAC before handing a
 * frame to the CPU port: 00 <port bitmap> 00 00.  The real switch strips them
 * on the wire; we have no switch, so strip them here.  Without this the
 * netinstall discovery broadcast (00 50 00 00 08 00 ...) reaches the tap with
 * 0x0050 where an EtherType belongs and every listener drops it.
 *
 * Returns true if the frame should be sent, false if it was addressed to a
 * switch port other than the one this backend represents.
 */
static bool mt7621_tx_strip_tag(MT7621State *s, uint8_t *f, size_t *plen)
{
    size_t len = *plen;
    unsigned bitmap;

    if (len < 18 || f[12] != 0x00 || f[14] != 0x00 || f[15] != 0x00) {
        return true;                        /* untagged conduit frame */
    }
    bitmap = f[13];
    if (!(f[0] & 1) && bitmap && !(bitmap & (1u << s->eth_port))) {
        return false;                       /* another port owns it */
    }
    memmove(f + 12, f + 16, len - 16);
    *plen = len - 4;
    return true;
}

static void mt7621_eth_pdma_tx(MT7621State *s);

/*
 * MT7530 behind the MDIO bus: Linux talks to it through PHY address 0x1f
 * with a page/lo/hi protocol (mt7530_mii_read/write):
 *   write(0x1f, 0x1f, page); lo = read(0x1f, r); hi = read(0x1f, 0x10)
 * where page = (reg >> 6) & 0x3ff and r = (reg >> 2) & 0xf.
 */
static uint32_t *mt7621_sw_word(MT7621State *s, unsigned page, unsigned r)
{
    return &s->sw_regs[((page & 0x3ff) << 4) | (r & 0xf)];
}

static uint16_t mt7621_eth_phy_read(MT7621State *s, unsigned phy, unsigned reg)
{
    bool up;

    if (phy == 0x1f) {
        unsigned r = reg & 0x1f;

        if (r == 0x1f) {
            return 0xffff;      /* page register: data ignored on read */
        }
        if (r == 0x10) {
            uint32_t w = *mt7621_sw_word(s, s->sw_page, s->sw_r);

            s->sw_r = 0;
            return (w >> 16) & 0xffff;
        }
        s->sw_r = r;
        /* HWTRAP (0x7800) must never read back zero: the probe polls it. */
        if ((((unsigned)s->sw_page << 6) | ((unsigned)r << 2)) == 0x7800 &&
            *mt7621_sw_word(s, s->sw_page, r) == 0) {
            *mt7621_sw_word(s, s->sw_page, r) = 1;
        }
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: sw rd page=%03x r=%x -> %08x\n",
                    s->sw_page, r, *mt7621_sw_word(s, s->sw_page, r));
        }
        return *mt7621_sw_word(s, s->sw_page, r) & 0xffff;
    }

    /*
     * Only the switch port that owns the net backend has a cable in it.
     * The internal PHYs are addressed by port number (lan4 = 1 ... lan1 = 4)
     * and the wan PHY sits at address 0.
     *
     * A real PHY reports link only after autonegotiation finished; claiming
     * it on the very first poll makes genphy_read_status() resolve the link
     * once, before phylink programmed the advertisement, and it then
     * short-circuits every later poll, leaving the port at a stale speed.
     */
    /*
     * RouterBOOT 7.x checks the cable on a PHY that does not necessarily
     * match the port we bridge, and spins on MTK_PHY_IAC until it sees
     * link.  MT7621_ETH_ANYLINK reports link on every PHY, which is what
     * the tap actually represents while we model a single backend.
     */
    if (getenv("MT7621_ETH_ANYLINK")) {
        /*
         * Debug/etherboot knob: claim link on every PHY (including the WAN
         * PHY at address 0) and without the autoneg delay, so booters that
         * probe the wrong PHY, or probe only once, still see a cable.
         */
        up = true;
    } else {
        up = phy && phy == s->eth_port;
    }
    if (up && !getenv("MT7621_ETH_ANYLINK")) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (!s->phy_link_ns) {
            /*
             * Short, not instant: Linux's phylink programs the
             * advertisement and resolves the link on its first status read,
             * but RouterBOOT checks "is there a cable" only ~2 s into the
             * boot, before its DHCP phase, so the delay has to be well
             * below that.
             */
            s->phy_link_ns = now + 300 * 1000 * 1000LL;
        }
        up = now >= s->phy_link_ns;
    }

    switch (reg & 0x1f) {
    case 1:                             /* BMSR: live status */
        /* No autoneg: the PHY is strapped to 1000BASE-T full duplex. */
        return up ? 0x7965 : 0x7941;
    case 10:                            /* MII_STAT1000: link partner */
        return up ? 0x3800 : 0x0000;
    case 15:                            /* MII_ESTATUS: capabilities */
        return 0xc000;                  /* 1000BASE-T full/half */
    default:
        return s->phy_regs[phy & 0x1f][reg & 0x1f];
    }
}

static uint64_t mt7621_eth_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_ETH_SIZE - 1);
    uint32_t val = s->eth_regs[off >> 2];

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: read %04x -> %08x (size %u)\n",
                (unsigned)off, val, size);
    }
    return val;
}

static void mt7621_eth_write(void *opaque, hwaddr addr, uint64_t val64,
                             unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & (MT7621_ETH_SIZE - 1);
    uint32_t val = (uint32_t)val64;

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: write %04x <- %08x (size %u)\n",
                (unsigned)off, val, size);
    }

    if (off == ETH_QDMA_GLO_CFG) {
        s->qtx_en = !!(val & 1);
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_PTX_BASE_PTR0 || off == ETH_PTX_MAX_CNT0 ||
        off == ETH_PTX_CTX_IDX0 || off == ETH_PDMA_GLO_CFG) {
        s->eth_regs[off >> 2] = val;
        if (off == ETH_PTX_BASE_PTR0) {
            s->ptx_idx = 0;
        }
        mt7621_eth_pdma_tx(s);
        return;
    }
    if (off == ETH_QTX_DTX_PTR) {
        /*
         * The driver points this at the start of a freshly allocated ring
         * before enabling the DMA, so it is both the forward pointer and
         * the ring base.
         */
        s->qtx_dtx = val;
        s->qtx_base = val;
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_QTX_DRX_PTR) {
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_QTX_CTX_PTR) {
        s->eth_regs[off >> 2] = val;
        /* Kicks that arrive while the engine is off (ring setup) are
         * ignored, exactly like the hardware does. */
        if (s->qtx_en) {
            mt7621_eth_tx(s, val);
        }
        return;
    }
    if (off == ETH_PDMA_INT_STATUS) {
        s->eth_regs[off >> 2] &= ~val;     /* write 1 to clear */
        mt7621_eth_update_irq(s);
        return;
    }
    if (off == ETH_QDMA_INT_STATUS) {
        s->eth_regs[off >> 2] &= ~val;
        mt7621_eth_update_irq(s);
        return;
    }
    if (off == ETH_PRX_BASE_PTR0) {
        /*
         * Only the base pointer changes here.  RouterBOOT rewrites it for
         * every descriptor it hands back, so resetting the DMA index would
         * make the engine restart at descriptor 0 each time and drop frames
         * into descriptors the bootloader still owns.
         */
        s->eth_regs[off >> 2] = val;
        return;
    }
    if (off == ETH_PDMA_GLO_CFG) {
        uint32_t old = s->eth_regs[off >> 2];

        s->eth_regs[off >> 2] = val;
        /* RX DMA (re)start: the engine begins at descriptor 0 again */
        if ((val & 0x4) && !(old & 0x4)) {
            s->rx_drx_idx = 0;
            s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = 0;
        }
        return;
    }
    if (off == 0x0a08) {                /* PDMA_RST_IDX */
        s->eth_regs[off >> 2] = val;
        if (val & (1u << 16)) {         /* RST_DRX_IDX0 */
            s->rx_drx_idx = 0;
            s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = 0;
        }
        return;
    }
    if (off == ETH_PRX_MAX_CNT0) {
        s->eth_regs[off >> 2] = val;
        if (s->rx_drx_idx >= val) {
            s->rx_drx_idx = 0;
        }
        return;
    }
    if (off == ETH_QDMA_INT_MASK || off == ETH_PDMA_INT_MASK) {
        /*
         * A level-triggered line must drop as soon as the driver masks the
         * source, otherwise the CPU stays in the interrupt handler forever.
         */
        s->eth_regs[off >> 2] = val;
        mt7621_eth_update_irq(s);
        return;
    }

    if (off == MT7621_ETH_IAC &&
        (val & ETH_IAC_ACCESS) && (val & ETH_IAC_START)) {
        unsigned phy = (val >> ETH_IAC_ADDR_SHIFT) & 0x1f;
        unsigned reg = (val >> ETH_IAC_REG_SHIFT) & 0x1f;

        if (val & ETH_IAC_WRITE) {
            unsigned data = val & 0xffff;

            if (phy == 0x1f) {
                if (reg == 0x1f) {
                    s->sw_page = data & 0x3ff;
                } else if (reg == 0x10) {
                    uint32_t *w = mt7621_sw_word(s, s->sw_page, s->sw_r);

                    *w = (*w & 0xffff) | (data << 16);
                    /*
                     * MT7530_ATC (0x90): the driver starts an address-table
                     * operation and polls ATC_BUSY (bit 31) until it clears.
                     * Complete it immediately.
                     */
                    {
                        unsigned regno = ((unsigned)s->sw_page << 6) |
                                         ((unsigned)s->sw_r << 2);

                        if (regno == 0x80) {
                            *w &= ~0x00008000u;  /* MT7530_ATC: ATC_BUSY */
                        } else if (regno == 0x90) {
                            *w &= ~0x80000000u;
                        }
                    }
                } else {
                    uint32_t *w;

                    s->sw_r = reg;
                    w = mt7621_sw_word(s, s->sw_page, reg);
                    *w = (*w & 0xffff0000) | data;
                }
            }
            val &= ~ETH_IAC_ACCESS;
        } else {
            val = (val & ~(ETH_IAC_ACCESS | 0xffff)) |
                  mt7621_eth_phy_read(s, phy, reg);
        }
    }

    s->eth_regs[off >> 2] = val;
}

/*
 * PDMA transmit ring (0x1e100800).  RouterBOOT drives this one, Linux uses
 * QDMA instead, so both paths have to work.  Descriptors are handed to the
 * engine with DDONE clear and a length in SDL0; the engine clears it and
 * sets DDONE when the frame is gone.
 */
static void mt7621_eth_pdma_tx(MT7621State *s)
{
    uint32_t base = s->eth_regs[ETH_PTX_BASE_PTR0 / 4];
    uint32_t cnt = s->eth_regs[ETH_PTX_MAX_CNT0 / 4];
    uint32_t ctx = s->eth_regs[ETH_PTX_CTX_IDX0 / 4];
    uint32_t idx = s->ptx_idx;
    bool processed = false;
    uint8_t frame[2048];
    size_t flen = 0;
    unsigned fport = 0;
    int guard = 0;

    if (!base || !cnt || cnt > 0x1000) {
        s->ptx_idx = ctx;
        return;
    }
    if (idx >= cnt) {
        idx = 0;
    }
    if (ctx >= cnt) {
        ctx = 0;
    }

    while (idx != ctx && guard++ <= (int)cnt) {
        uint32_t p = base + idx * 16;
        uint32_t addr0 = address_space_ldl_le(&address_space_memory, p,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t info2 = address_space_ldl_le(&address_space_memory, p + 4,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t addr1 = address_space_ldl_le(&address_space_memory, p + 8,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t info4 = address_space_ldl_le(&address_space_memory, p + 12,
                                              MEMTXATTRS_UNSPECIFIED, NULL);
        unsigned len0 = PTX_SDL0(info2);
        unsigned len1 = (info2 & PTX_LS1) ? PTX_SDL1(info2) : 0;
        bool last = !!(info2 & PTX_LS0);

        if (info2 & PTX_DDONE) {
            /* descriptor not handed to the DMA: nothing else to do */
            break;
        }
        if (flen == 0) {
            fport = PTX_FPORT(info4);
        }
        if (addr0 && len0 && flen + len0 <= sizeof(frame)) {
            address_space_read(&address_space_memory, addr0,
                               MEMTXATTRS_UNSPECIFIED, frame + flen, len0);
            flen += len0;
        }
        if (addr1 && len1 && flen + len1 <= sizeof(frame)) {
            address_space_read(&address_space_memory, addr1,
                               MEMTXATTRS_UNSPECIFIED, frame + flen, len1);
            flen += len1;
        }

        if (last && flen >= 14) {
            bool forward = true;

            forward = mt7621_tx_strip_tag(s, frame, &flen);
            if (getenv("MT7621_ETHLOG")) {
                fprintf(stderr, "eth: ptx idx=%u fport=%u tag=%02x%02x "
                        "len=%zu dst=%02x:%02x:%02x:%02x:%02x:%02x fwd=%d",
                        idx, fport, frame[12], frame[13], flen, frame[0],
                        frame[1], frame[2], frame[3], frame[4], frame[5],
                        forward);
                if (flen >= 34 && frame[12] == 0x08 && frame[13] == 0x00) {
                    fprintf(stderr, " sport=%u dport=%u proto=%u",
                            (frame[34] << 8) | frame[35],
                            (frame[36] << 8) | frame[37], frame[23]);
                }
                fprintf(stderr, "\n");
            }
            if (getenv("MT7621_ETHLOG") && flen >= 300 &&
                frame[12] == 0x08 && frame[13] == 0x00 &&
                frame[23] == 0x11 &&
                (((frame[34] << 8) | frame[35]) == 68 ||
                 ((frame[36] << 8) | frame[37]) == 67 ||
                 ((frame[34] << 8) | frame[35]) == 67 ||
                 ((frame[36] << 8) | frame[37]) == 68)) {
                unsigned i;
                fprintf(stderr, "eth: bootp len=%zu:", flen);
                for (i = 42; i < flen && i < 420; i++) {
                    fprintf(stderr, " %02x", frame[i]);
                }
                fprintf(stderr, "\n");
            }
            {
                static unsigned dump_budget = 40;
                if (getenv("MT7621_ETHLOG") && dump_budget && flen >= 60) {
                    unsigned i, n = flen < 96 ? (unsigned)flen : 96;
                    dump_budget--;
                    fprintf(stderr, "eth: dump len=%zu:", flen);
                    for (i = 0; i < n; i++) {
                        fprintf(stderr, " %02x", frame[i]);
                    }
                    fprintf(stderr, "\n");
                }
            }
            if (forward && s->nic) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, flen);
            }
            flen = 0;
        }

        address_space_stl_le(&address_space_memory, p + 4,
                             info2 | PTX_DDONE, MEMTXATTRS_UNSPECIFIED, NULL);
        processed = true;
        idx = (idx + 1) % cnt;
    }

    s->ptx_idx = idx;
    s->eth_regs[ETH_PTX_DTX_IDX0 / 4] = idx;
    /*
     * Tell the driver that the descriptors it handed us are free again.
     * Linux masks TX_DONE_INT0 and reclaims in its interrupt handler; with
     * no interrupt every queue fills up to max_tx_buffers_per_port and the
     * stack times out, restarts the switch and retries forever.
     */
    /*
     * Only report completions once the guest has enabled PDMA interrupts.
     * RouterBOOT polls PDMA_INT_STATUS without ever setting the mask, and an
     * unexpected status bit wedges its TFTP state machine (the transfer
     * crawls through 1400 retries instead of completing).
     */
    if (processed && s->eth_regs[ETH_PDMA_INT_MASK / 4]) {
        s->eth_regs[ETH_PDMA_INT_STATUS / 4] |= PDMA_TX_DONE0;
        mt7621_eth_update_irq(s);
    }
    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: ptx done idx=%u ctx=%u\n", idx, ctx);
    }
}

static void mt7621_eth_update_irq(MT7621State *s)
{
    uint32_t pdma = s->eth_regs[ETH_PDMA_INT_STATUS / 4] &
                    s->eth_regs[ETH_PDMA_INT_MASK / 4];
    uint32_t qdma = s->eth_regs[ETH_QDMA_INT_STATUS / 4] &
                    s->eth_regs[ETH_QDMA_INT_MASK / 4];

    qemu_set_irq(s->eth_irq, (pdma | qdma) != 0);
}

/* Move one received frame into the next PDMA descriptor. */
static void mt7621_eth_rx(MT7621State *s, const uint8_t *buf, size_t len)
{
    uint32_t base = s->eth_regs[ETH_PRX_BASE_PTR0 / 4];
    uint32_t cnt = s->eth_regs[ETH_PRX_MAX_CNT0 / 4];
    uint32_t idx = s->eth_regs[ETH_PRX_DRX_IDX0 / 4];
    uint32_t desc, rxd1, rxd2, cap, crx;
    uint8_t tagged[1600];
    size_t flen;
    unsigned rx_off;
    bool took_bql = !bql_locked();
    static int dbg;

    if (!base || !cnt || len < 14) {
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: rx drop no-ring base=%08x cnt=%u len=%zu\n",
                    base, cnt, len);
        }
        return;
    }
    if (idx >= cnt) {
        idx = 0;
    }

    /*
     * The engine writes the descriptor DRX_IDX points at and then advances
     * it.  A descriptor the software has not consumed yet still carries the
     * DDONE flag, which is the only reliable "ring full" test: RouterBOOT
     * keeps CRX_IDX at the index it just freed, so comparing the two
     * indices (as the Linux driver's convention suggests) drops every frame
     * after the first one.
     */
    crx = s->eth_regs[ETH_PRX_CRX_IDX0 / 4];
    {
        uint32_t busy = address_space_ldl_le(&address_space_memory,
                                             base + idx * 16 + 4,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        if (busy & RX_DMA_DONE) {
            if (getenv("MT7621_ETHLOG")) {
                fprintf(stderr, "eth: rx drop ring-full idx=%u crx=%u\n",
                        idx, crx);
            }
            return;
        }
    }

    if (took_bql) {
        bql_lock();
    }

    desc = base + idx * 16;
    rxd1 = address_space_ldl_le(&address_space_memory, desc,
                                MEMTXATTRS_UNSPECIFIED, NULL);
    rxd2 = address_space_ldl_le(&address_space_memory, desc + 4,
                                MEMTXATTRS_UNSPECIFIED, NULL);
    /* Words 16:29 of a clean descriptor hold the buffer capacity. */
    cap = (rxd2 >> 16) & 0x3fff;
    if (!cap) {
        cap = 1536;
    }
    if (!rxd1) {
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: rx drop no-buffer idx=%u desc=%08x\n",
                    idx, desc);
        }
        goto out;
    }

    /*
     * Linux drives the port as a DSA master: the switch inserts its special
     * tag after the source MAC and the engine starts the frame two bytes
     * into the buffer (RX_2B_OFFSET).  RouterBOOT configures the switch
     * without the tag and with RX_2B_OFFSET clear, so honour that bit
     * instead of always building a tagged frame.
     */
    if (s->eth_regs[ETH_PDMA_GLO_CFG / 4] & (1u << 31)) {
        if (len + 4 > sizeof(tagged)) {
            len = sizeof(tagged) - 4;
        }
        memcpy(tagged, buf, 12);
        tagged[12] = 0x00;                /* untagged special tag */
        tagged[13] = s->eth_port & 0x3f;  /* source port */
        tagged[14] = 0x00;
        tagged[15] = 0x00;
        memcpy(tagged + 16, buf + 12, len - 12);
        flen = len + 4;
        rx_off = 2;
    } else {
        flen = len;
        if (flen > sizeof(tagged)) {
            flen = sizeof(tagged);
        }
        memcpy(tagged, buf, flen);
        rx_off = 0;
    }

    if (flen > cap) {
        flen = cap;                        /* stay inside the driver buffer */
    }
    /*
     * PDMA_GLO_CFG has RX_2B_OFFSET set, so the engine starts the frame two
     * bytes into the descriptor buffer; the driver's skb_reserve() accounts
     * for that offset.
     */
    address_space_write(&address_space_memory, rxd1 + rx_off,
                        MEMTXATTRS_UNSPECIFIED, tagged, flen);

    /* rxd2's DONE bit is the release flag, so write it last. */
    address_space_stl_le(&address_space_memory, desc + 8, 0,
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(&address_space_memory, desc + 12,
                         (1u << RX_DMA_FPORT_SHIFT),   /* fport 1 = GMAC1 */
                         MEMTXATTRS_UNSPECIFIED, NULL);
    address_space_stl_le(&address_space_memory, desc + 4,
                         RX_DMA_DONE | RX_DMA_PLEN0(flen),
                         MEMTXATTRS_UNSPECIFIED, NULL);

    s->rx_drx_idx = (idx + 1) % cnt;
    s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = s->rx_drx_idx;
    s->eth_regs[ETH_PDMA_INT_STATUS / 4] |= PDMA_RX_DONE;

    {
        static unsigned big_budget = 6;
        if (getenv("MT7621_ETHLOG") && big_budget && len >= 1000) {
            unsigned i, n = flen < 52 ? (unsigned)flen : 52;
            big_budget--;
            fprintf(stderr, "eth: rxdump len=%zu:", flen);
            for (i = 0; i < n; i++) {
                fprintf(stderr, " %02x", tagged[i]);
            }
            fprintf(stderr, "\n");
        }
    }
    dbg++;
    if (getenv("MT7621_ETHLOG") && dbg < 16) {
        uint32_t prev = desc - 16;
        uint32_t prev_rxd2 = address_space_ldl_le(&address_space_memory,
                                                  prev + 4,
                                                  MEMTXATTRS_UNSPECIFIED, NULL);

        fprintf(stderr, "eth: rx[%d] desc=%08x len=%zu/%u idx=%u crx=%u "
                "proto=%02x%02x dst=%02x:%02x:%02x:%02x:%02x:%02x "
                "prev_done=%d\n",
                dbg, desc, flen, cap, idx, crx, tagged[12], tagged[13],
                tagged[0], tagged[1], tagged[2], tagged[3], tagged[4],
                tagged[5], !!(prev_rxd2 & RX_DMA_DONE));
    }

    mt7621_eth_update_irq(s);

    /*
     * The NIC always has room, so tell the net core to retry anything the
     * backend queued while the core was busy.
     */
    qemu_flush_queued_packets(qemu_get_queue(s->nic));

out:
    if (took_bql) {
        bql_unlock();
    }
}

static bool mt7621_eth_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t mt7621_eth_receive(NetClientState *nc, const uint8_t *buf,
                                  size_t size)
{
    MT7621State *s = qemu_get_nic_opaque(nc);

    mt7621_eth_rx(s, buf, size);
    return size;
}

static void mt7621_eth_cleanup(NetClientState *nc)
{
}

static NetClientInfo mt7621_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mt7621_eth_can_receive,
    .receive = mt7621_eth_receive,
    .cleanup = mt7621_eth_cleanup,
};

/*
 * Transmit: the Linux driver enqueues descriptors and then writes the
 * physical address of the newest one to MTK_QTX_CTX_PTR.  Walk the chain
 * from the last completed descriptor, hand each buffer to the net backend
 * and publish the new done pointer.
 */
/*
 * The driver leaves TCP/UDP checksums to the MAC (NETIF_F_HW_CSUM): the
 * stack fills the checksum field with the pseudo-header sum and sets
 * TX_DMA_CHKSUM in txd4.  Finish the job here, otherwise every TCP segment
 * leaves with a bogus checksum and the peer silently drops it.
 */
static void mt7621_eth_tx_csum(uint8_t *frame, size_t len)
{
    size_t l3 = 14, l4 = 0, l4_len = 0, csum_off;
    unsigned eth, proto, ihl, tot;

    if (len < 34) {
        return;
    }
    eth = (frame[12] << 8) | frame[13];
    if (eth == 0x8100 || eth == 0x88a8) {
        if (len < 38) {
            return;
        }
        eth = (frame[16] << 8) | frame[17];
        l3 = 18;
    }

    if (eth == 0x0800) {                    /* IPv4 */
        if (len < l3 + 20 || (frame[l3] >> 4) != 4) {
            return;
        }
        ihl = (frame[l3] & 0xf) * 4;
        if (ihl < 20 || len < l3 + ihl) {
            return;
        }
        if (((frame[l3 + 6] << 8) | frame[l3 + 7]) & 0x1fff) {
            return;                         /* fragment: checksum is final */
        }
        tot = (frame[l3 + 2] << 8) | frame[l3 + 3];
        if (tot < ihl) {
            return;
        }
        proto = frame[l3 + 9];
        l4 = l3 + ihl;
        l4_len = tot - ihl;
    } else if (eth == 0x86dd) {             /* IPv6 */
        if (len < l3 + 40 || (frame[l3] >> 4) != 6) {
            return;
        }
        proto = frame[l3 + 6];
        l4 = l3 + 40;
        l4_len = (frame[l3 + 4] << 8) | frame[l3 + 5];
    } else {
        return;
    }

    switch (proto) {
    case 6:                                 /* TCP */
    case 17:                                /* UDP */
        csum_off = (proto == 6) ? 16 : 6;
        break;
    case 1:                                 /* ICMP */
    case 58:                                /* ICMPv6 */
        csum_off = 2;
        break;
    default:
        return;                             /* header with extensions, etc. */
    }

    if (len < l4 + csum_off + 2) {
        return;
    }
    if (l4_len > len - l4) {
        l4_len = len - l4;                  /* truncated: sum what we have */
    }

    {
        uint32_t sum = 0;
        size_t i;
        uint16_t c;

        for (i = 0; i + 1 < l4_len; i += 2) {
            sum += (frame[l4 + i] << 8) | frame[l4 + i + 1];
        }
        if (l4_len & 1) {
            sum += frame[l4 + l4_len - 1] << 8;
        }
        while (sum >> 16) {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        c = ~sum;
        if (!c) {
            c = 0xffff;
        }
        frame[l4 + csum_off] = c >> 8;
        frame[l4 + csum_off + 1] = c & 0xff;
    }
}

/*
 * TSO: the driver hands us one oversized TCP segment and puts the MSS in the
 * checksum field (mtk_start_xmit does tcp_hdr(skb)->check = gso_size).  Split
 * it into MTU-sized frames, fixing up the IP length/id, the sequence number
 * and both checksums.
 */
static void mt7621_eth_tx_tso(MT7621State *s, const uint8_t *frame, size_t len)
{
    uint8_t out[1600];
    size_t l3 = 14;
    unsigned eth, proto, ihl, doff, hdr, payload_len, mss, off, room;

    if (len < 54) {
        return;
    }
    eth = (frame[12] << 8) | frame[13];
    if (eth == 0x8100 || eth == 0x88a8) {
        if (len < 58) {
            return;
        }
        eth = (frame[16] << 8) | frame[17];
        l3 = 18;
    }
    if (eth != 0x0800 && eth != 0x86dd) {
        return;
    }
    if (eth == 0x0800) {
        ihl = (frame[l3] & 0xf) * 4;
        proto = frame[l3 + 9];
        payload_len = ((frame[l3 + 2] << 8) | frame[l3 + 3]) - ihl;
    } else {
        ihl = 40;
        proto = frame[l3 + 6];
        payload_len = (frame[l3 + 4] << 8) | frame[l3 + 5];
    }
    if (proto != 6 || ihl < 20 || len < l3 + ihl + 20) {
        return;
    }
    doff = ((frame[l3 + ihl + 12] >> 4) & 0xf) * 4;
    if (doff < 20 || l3 + ihl + doff > len) {
        return;
    }
    if (payload_len > len - l3 - ihl - doff) {
        payload_len = len - l3 - ihl - doff;
    }

    mss = (frame[l3 + ihl + 16] << 8) | frame[l3 + ihl + 17];
    room = (eth == 0x0800) ? (1514 - l3 - ihl - doff)
                           : (1514 - l3 - ihl - doff);
    if (mss == 0 || mss > room) {
        mss = room;
    }
    if ((int)room <= 0) {
        return;
    }
    hdr = l3 + ihl + doff;

    for (off = 0; off < payload_len; off += mss) {
        unsigned chunk = payload_len - off;
        bool last;
        uint32_t sum = 0;
        size_t i;
        uint16_t c;

        if (chunk > mss) {
            chunk = mss;
        }
        last = (off + chunk >= payload_len);

        memcpy(out, frame, hdr);
        memcpy(out + hdr, frame + hdr + off, chunk);

        /* IPv4 header: total length, id and checksum */
        if (eth == 0x0800) {
            out[l3 + 2] = (ihl + doff + chunk) >> 8;
            out[l3 + 3] = (ihl + doff + chunk) & 0xff;
            sum = 0;
            for (i = 0; i < ihl; i += 2) {
                sum += (out[l3 + i] << 8) | out[l3 + i + 1];
            }
            while (sum >> 16) {
                sum = (sum & 0xffff) + (sum >> 16);
            }
            c = ~sum;
            out[l3 + 10] = c >> 8;
            out[l3 + 11] = c & 0xff;
        }
        /* TCP sequence number advances with the payload */
        {
            uint32_t seq = ((uint32_t)out[l3 + ihl + 4] << 24) |
                           (out[l3 + ihl + 5] << 16) |
                           (out[l3 + ihl + 6] << 8) | out[l3 + ihl + 7];

            seq += off;
            out[l3 + ihl + 4] = seq >> 24;
            out[l3 + ihl + 5] = seq >> 16;
            out[l3 + ihl + 6] = seq >> 8;
            out[l3 + ihl + 7] = seq;
        }
        /* PSH only on the final segment */
        out[l3 + ihl + 12] = (out[l3 + ihl + 12] & 0xf0) | (doff / 4);
        if (last) {
            out[l3 + ihl + 13] |= 0x08;
        } else {
            out[l3 + ihl + 13] &= ~0x08;
        }
        /* TCP checksum over pseudo header + segment */
        out[l3 + ihl + 16] = 0;
        out[l3 + ihl + 17] = 0;
        sum = 0;
        if (eth == 0x0800) {
            sum += (out[l3 + 12] << 8) | out[l3 + 13];
            sum += (out[l3 + 14] << 8) | out[l3 + 15];
            sum += (out[l3 + 16] << 8) | out[l3 + 17];
            sum += (out[l3 + 18] << 8) | out[l3 + 19];
        } else {
            for (i = 8; i < 40; i += 2) {
                sum += (out[l3 + i] << 8) | out[l3 + i + 1];
            }
        }
        sum += 6;
        sum += doff + chunk;
        for (i = 0; i < doff + chunk; i += 2) {
            unsigned hi = out[l3 + ihl + i];
            unsigned lo = (i + 1 < doff + chunk) ?
                          out[l3 + ihl + i + 1] : 0;

            sum += (hi << 8) | lo;
        }
        while (sum >> 16) {
            sum = (sum & 0xffff) + (sum >> 16);
        }
        c = ~sum;
        if (!c) {
            c = 0xffff;
        }
        out[l3 + ihl + 16] = c >> 8;
        out[l3 + ihl + 17] = c & 0xff;

        if (s->nic) {
            qemu_send_packet(qemu_get_queue(s->nic), out, hdr + chunk);
        }
    }
}

static void mt7621_eth_tx(MT7621State *s, uint32_t ctx)
{
    uint32_t p = s->qtx_dtx;
    uint32_t base = s->qtx_base;
    int guard = 0;
    unsigned sent = 0;
    /*
     * One frame can span several descriptors (skb head + page fragments);
     * the engine concatenates them until it meets TX_DMA_LS0.  Keep the
     * partially assembled frame across calls.
     */
    static uint8_t frame[0x10000];
    static size_t flen;
    static bool frame_started;
    static unsigned frame_fport;
    static bool frame_csum;
    static bool frame_tso;

    if (!base || p < base || p >= base + TX_RING_BYTES ||
        ctx < base || ctx >= base + TX_RING_BYTES) {
        /* the ring was re-allocated: resynchronise and send nothing */
        flen = 0;
        frame_started = false;
        s->qtx_dtx = ctx;
        s->qtx_base = ctx;
        return;
    }

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: tx ctx=%08x start=%08x\n", ctx, p);
    }

    while (p != ctx && guard++ < TX_RING_BYTES / 16) {
        uint32_t txd1 = address_space_ldl_le(&address_space_memory, p,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t txd2 = address_space_ldl_le(&address_space_memory, p + 4,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t txd3 = address_space_ldl_le(&address_space_memory, p + 8,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        uint32_t txd4 = address_space_ldl_le(&address_space_memory, p + 12,
                                             MEMTXATTRS_UNSPECIFIED, NULL);
        unsigned len = (txd3 >> 16) & 0x3fff;
        bool filled = !(txd3 & TX_DMA_OWNER_CPU);
        bool last = !!(txd3 & TX_DMA_LS0);

        if (filled && txd1 && len) {
            if (!frame_started) {
                frame_started = true;
                frame_fport = (txd4 >> TX_DMA_FPORT_SHIFT) & 0x7;
                frame_csum = !!(txd4 & TX_DMA_CHKSUM);
                frame_tso = !!(txd4 & TX_DMA_TSO);
            }
            if (len > sizeof(frame) - flen) {
                len = sizeof(frame) - flen;
            }
            address_space_read(&address_space_memory, txd1,
                               MEMTXATTRS_UNSPECIFIED, frame + flen, len);
            flen += len;

            if (last) {
                bool forward = false;

                /*
                 * Frames sent through GMAC1 (the DSA master) carry the MTK
                 * special tag at offset 12: byte 12 is the tag type and
                 * byte 13 the destination port bitmap.  Only frames the
                 * switch would put on our port may leave through this
                 * backend; GMAC2 (the wan MAC) is not behind the switch.
                 */
                if (frame_fport == 1) {
                    forward = mt7621_tx_strip_tag(s, frame, &flen);
                }

                if (forward && frame_tso) {
                    mt7621_eth_tx_tso(s, frame, flen);
                    forward = false;
                } else if (forward && frame_csum) {
                    mt7621_eth_tx_csum(frame, flen);
                }

                if (!forward && getenv("MT7621_ETHLOG") && flen > 18) {
                    fprintf(stderr, "eth: tx drop fport=%u tag=%02x%02x "
                            "len=%zu dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                            frame_fport, frame[12], frame[13], flen,
                            frame[0], frame[1], frame[2], frame[3],
                            frame[4], frame[5]);
                }
                if (forward && getenv("MT7621_ETHLOG") && sent < 8) {
                    fprintf(stderr, "eth: txf dst=%02x:%02x:%02x:%02x:%02x:%02x "
                            "src=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x "
                            "len=%zu\n",
                            frame[0], frame[1], frame[2], frame[3], frame[4],
                            frame[5], frame[6], frame[7], frame[8], frame[9],
                            frame[10], frame[11], frame[12], frame[13], flen);
                }
                if (forward && s->nic) {
                    qemu_send_packet(qemu_get_queue(s->nic), frame, flen);
                    sent++;
                }
                flen = 0;
                frame_started = false;
            }
        }

        /* Hand the descriptor back to the CPU (TX_DMA_DONE). */
        if (filled) {
            address_space_stl_le(&address_space_memory, p + 8,
                                 txd3 | TX_DMA_OWNER_CPU,
                                 MEMTXATTRS_UNSPECIFIED, NULL);
        }
        p = txd2;
    }

    if (getenv("MT7621_ETHLOG")) {
        fprintf(stderr, "eth: tx done p=%08x ctx=%08x sent=%u guard=%d\n",
                p, ctx, sent, guard);
    }

    s->qtx_dtx = p;
    s->eth_regs[ETH_QTX_DRX_PTR / 4] = p;
    if (s->nic) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
    if (p == ctx) {
        s->eth_regs[ETH_QDMA_INT_STATUS / 4] |= QDMA_TX_DONE;
        mt7621_eth_update_irq(s);
    }
}

static const MemoryRegionOps mt7621_eth_ops = {
    .read = mt7621_eth_read,
    .write = mt7621_eth_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * NAND flash interface.
 *
 * The controller is modelled at the level the firmware drives it: commands
 * go to NFI_CMD, addresses to COLADDR/ROWADDR, and data is pulled out of
 * NFI_DATAR while NFI_PIO_DIRDY reports that the next byte is ready.  Status
 * is always "idle" because the emulated flash answers instantly.
 *
 * The backing store is the same image passed with -kernel, which is the raw
 * NAND dump: page 2048 bytes, OOB 64 bytes, 64 pages per block.  The dump
 * holds only the main area, so the OOB is synthesised all-0xff, i.e. every
 * block good.
 */
static void mt7621_nfi_fifo_put(MT7621State *s, const void *data, size_t len)
{
    if (len > sizeof(s->nfi_fifo)) {
        len = sizeof(s->nfi_fifo);
    }
    memcpy(s->nfi_fifo, data, len);
    s->nfi_fifo_len = len;
    s->nfi_fifo_pos = 0;
}

/*
 * The Linux driver sends each address byte on its own (COLADDR followed by
 * ADDRNOB = 1); two column bytes and three row bytes make up a page
 * address.  Breed instead programs the count once, so fall back to the
 * COLADDR/ROWADDR fields when no byte stream was collected.
 */
static uint64_t mt7621_nfi_addr_col(MT7621State *s)
{
    if (s->nfi_addr_len >= 2) {
        s->nfi_col = s->nfi_addr[0] | (s->nfi_addr[1] << 8);
    }
    return s->nfi_col;
}

static uint64_t mt7621_nfi_addr_row(MT7621State *s)
{
    if (s->nfi_addr_len >= 3) {
        uint64_t row = s->nfi_addr[2];

        if (s->nfi_addr_len > 3) {
            row |= (uint64_t)s->nfi_addr[3] << 8;
        }
        if (s->nfi_addr_len > 4) {
            row |= (uint64_t)s->nfi_addr[4] << 16;
        }
        s->nfi_row = row;
    }
    return s->nfi_row;
}

static void mt7621_nfi_page_read(MT7621State *s)
{
    uint8_t buf[MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE];
    uint64_t off;

    memset(buf, 0xff, sizeof(buf));

    mt7621_nfi_addr_col(s);
    mt7621_nfi_addr_row(s);

    off = (uint64_t)s->nfi_row * s->nand_stride;
    if (s->nand && off < s->nand_size) {
        uint64_t n = s->nand_size - off;

        if (n > s->nand_stride) {
            n = s->nand_stride;
        }
        memcpy(buf, s->nand + off, n);     /* data + spare when present */
    }

    s->nfi_served = 0;
    mt7621_nfi_fifo_put(s, buf, sizeof(buf));

    /* Honour the column address: 2048..2111 selects the spare area. */
    if (s->nfi_col < sizeof(buf)) {
        s->nfi_fifo_pos = s->nfi_col;
    }
}

/*
 * Optional write-through to a block backend, so a session's NAND changes
 * survive QEMU restarts without touching the pristine dump.  Usage:
 *   -drive file=nand.img,if=none,id=nand,format=raw
 * The image is seeded from the -kernel dump the first time it is used.
 */
static int mt7621_pwrite_all(int fd, const void *buf, size_t len, off_t off)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = pwrite(fd, p, len, off);

        if (n <= 0) {
            return -1;
        }
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

static void mt7621_nand_persist(MT7621State *s, uint64_t off,
                                const void *data, uint64_t len)
{
    bool failed;

    if (off + len > s->nand_size) {
        return;
    }
    if (s->nand_fd >= 0) {
        failed = mt7621_pwrite_all(s->nand_fd, data, len, off) < 0;
    } else if (s->nand_blk) {
        failed = blk_pwrite(s->nand_blk, off, len, data, 0) < 0;
    } else {
        return;
    }
    if (failed && !s->nand_wr_error) {
        warn_report("mt7621: cannot write the NAND image, changes are "
                    "kept in RAM only");
        s->nand_wr_error = true;
    }
}

static void mt7621_wdt_arm(MT7621State *s)
{
    uint32_t ctl = s->sysctl_regs[(MT7621_WDT_OFF + WDT_TMR1CTL) / 4];
    uint32_t load = s->sysctl_regs[(MT7621_WDT_OFF + WDT_TMR1LOAD) / 4];
    uint32_t prescale = (ctl >> 16) & 0xffff;
    int64_t ns;

    if (!(ctl & WDT_TMR1CTL_ENABLE) || load == 0) {
        timer_del(s->wdt_timer);
        return;
    }
    if (prescale == 0) {
        prescale = 1;
    }
    ns = (int64_t)load * prescale * 1000;      /* ticks of prescale usec */
    /* test hook: force a short bark to exercise the reset path */
    if (getenv("MT7621_WDT_TEST_MS")) {
        ns = strtoull(getenv("MT7621_WDT_TEST_MS"), NULL, 0) * 1000000LL;
    }
    timer_mod(s->wdt_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
    if (getenv("MT7621_WDTLOG")) {
        fprintf(stderr, "wdt: arm ctl=%08x load=%08x prescale=%u ns=%lld\n",
                ctl, load, prescale, (long long)ns);
    }
}

static void mt7621_wdt_expire(void *opaque)
{
    MT7621State *s = opaque;

    warn_report("mt7621: watchdog expired, resetting the machine");
    s->wdt_rst_cause = true;
    if (getenv("MT7621_NO_RESET")) {
        return;
    }
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static uint64_t mt7621_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t val = s->sysctl_regs[(MT7621_WDT_OFF +
                                   (addr & (MT7621_WDT_SIZE - 1))) / 4];

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: wdt read  %03x -> %08x\n",
                      (unsigned)(addr & (MT7621_WDT_SIZE - 1)), val);
    }
    return val;
}

static void mt7621_wdt_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MT7621State *s = opaque;
    hwaddr off = addr & (MT7621_WDT_SIZE - 1);

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: wdt write %03x <- %08x\n",
                      (unsigned)off, (uint32_t)val);
    }
    if (getenv("MT7621_WDTLOG")) {
        fprintf(stderr, "wdt: write %03x <- %08x (pc=%08x)\n",
                (unsigned)off, (uint32_t)val,
                s->cpu ? (uint32_t)s->cpu->env.active_tc.PC : 0);
    }
    s->sysctl_regs[(MT7621_WDT_OFF + off) / 4] = val;

    switch (off) {
    case WDT_TMRSTAT:
        if (val & WDT_TMRSTAT_RESTART) {
            mt7621_wdt_arm(s);
        }
        break;
    case WDT_TMR1CTL:
    case WDT_TMR1LOAD:
        mt7621_wdt_arm(s);
        break;
    }
}

static const MemoryRegionOps mt7621_wdt_ops = {
    .read = mt7621_wdt_read,
    .write = mt7621_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * SPI NOR controller (0x1e000b00).  Register layout, per the Linux and
 * U-Boot drivers:
 *
 *   0x00 TRANS    bit 8 start, bit 16 busy, [3:0] command bytes,
 *                 bit 6 = capture a 4-byte RX word (RouterBOOT style)
 *   0x04 OPCODE   up to 4 command bytes, big-endian
 *   0x08 DATA0..  FIFO, little-endian bytes
 *   0x28 MASTER   clock select / more-buffer mode
 *   0x2c MOREBUF  command/MISO/MOSI bit counts (Linux style)
 *   0x38 POLAR    chip select (written 0 to deassert)
 *   0x3c SPACE    dummy clocks
 *
 * The flash itself is a Winbond W25Q128FV (EF 40 18), matching the ID in
 * the board's hard configuration.
 */
#define MT7621_SPI_OFF          0x00000b00
#define SPI_TRANS               0x00
#define SPI_OPCODE              0x04
#define SPI_DATA0               0x08
#define SPI_MASTER              0x28
#define SPI_MOREBUF             0x2c
#define SPI_POLAR               0x38
#define SPI_SPACE               0x3c

#define SPI_TRANS_START         (1u << 8)
#define SPI_TRANS_BUSY          (1u << 16)
#define SPI_TRANS_RXWORD        (1u << 6)

static const uint8_t mt7621_spi_id[3] = { 0xef, 0x40, 0x18 };

static uint8_t mt7621_flash_read_byte(MT7621State *s, uint32_t addr)
{
    if (s->flash && addr < s->flash_size) {
        return s->flash[addr];
    }
    return 0xff;
}

static void mt7621_flash_write_bytes(MT7621State *s, uint32_t addr,
                                     const uint8_t *buf, int len)
{
    int i;

    if (!s->flash) {
        return;
    }
    for (i = 0; i < len; i++) {
        if (addr + i < s->flash_size) {
            s->flash[addr + i] &= buf[i];   /* NOR can only clear bits */
        }
    }
}

static void mt7621_flash_erase(MT7621State *s, uint32_t addr, uint32_t len)
{
    uint32_t start = addr & ~(len - 1);

    if (s->flash && start + len <= s->flash_size) {
        memset(s->flash + start, 0xff, len);
    }
}

/*
 * Clock the given MOSI bytes through the flash and fill miso_out with what
 * the chip returns.  bytes_out is advanced only for the RX phase (bytes
 * after cmd_len), which is what the controller captures into the FIFO.
 */
static void mt7621_spi_flash_run(MT7621State *s, const uint8_t *mosi,
                                 int n, int cmd_len, uint8_t *miso, int rx_len)
{
    int i;
    int pos = 0;
    int rx_pos = cmd_len;
    uint32_t args[8];
    int nargs = 0;

    memset(miso, 0xff, rx_len);

    if (getenv("MT7621_SPILOG")) {
        fprintf(stderr, "spi: op %02x n=%d cmd=%d rx=%d bytes=",
                n > 0 ? mosi[0] : -1, n, cmd_len, rx_len);
        for (i = 0; i < n; i++) {
            fprintf(stderr, "%02x", mosi[i]);
        }
        fprintf(stderr, "\n");
    }

    if (n <= 0) {
        /* A read-only transaction continuing a READ command. */
        if (s->spi_read_mode) {
            for (i = 0; i < rx_len; i++) {
                miso[i] = mt7621_flash_read_byte(s, s->spi_addr++);
            }
        }
        return;
    }

    /* The opcode is the first byte; the rest is address / data. */
    for (i = 1; i < n && nargs < (int)ARRAY_SIZE(args); i++) {
        args[nargs++] = mosi[i];
    }

    switch (mosi[0]) {
    case 0x9f:                          /* RDID */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            miso[i] = i < 3 ? mt7621_spi_id[i] : 0xff;
        }
        break;
    case 0x4b:                          /* RDUID (Winbond): 64-bit UID */
        /*
         * flash_get_uid() reads nine bytes - one dummy followed by the ID -
         * and prints them as a little-endian u64, so emit the dummy and then
         * the ID least-significant byte first.
         */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            if (i == 0) {
                miso[i] = 0xff;
            } else if (i <= 8) {
                miso[i] = (uint8_t)(s->spi_uid >> (8 * (i - 1)));
            } else {
                miso[i] = 0xff;
            }
        }
        break;
    case 0x90:                          /* manufacturer + device ID */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            miso[i] = i == 0 ? 0xef : (i == 1 ? 0x17 : 0x00);
        }
        break;
    case 0x05:                          /* RDSR */
    case 0x35:                          /* RDSR2 */
        s->spi_read_mode = false;
        for (i = 0; i < rx_len; i++) {
            miso[i] = 0x00;             /* not busy, no protection */
        }
        break;
    case 0x03:                          /* READ */
    case 0x0b: {                        /* FAST READ */
        static int rdlog_budget = 8;
        uint32_t addr = 0;

        for (i = 0; i < 3 && i < nargs; i++) {
            addr = (addr << 8) | args[i];
        }
        s->spi_addr = addr;
        s->spi_read_mode = true;
        rx_pos = mosi[0] == 0x0b ? 5 : 4;
        for (i = 0; i < rx_len; i++) {
            miso[i] = mt7621_flash_read_byte(s, s->spi_addr++);
        }
        if (getenv("MT7621_SPILOG") && rdlog_budget-- > 0) {
            fprintf(stderr, "spi: read op=%02x addr=%06x len=%d -> %02x %02x "
                    "%02x %02x %02x %02x\n", mosi[0], addr, rx_len,
                    miso[0], miso[1], miso[2], miso[3], miso[4], miso[5]);
        }
        break;
    }
    case 0x02:                          /* page program */
    case 0x01:                          /* write status */
    case 0x06:                          /* write enable */
    case 0x04:                          /* write disable */
    case 0x20:                          /* sector erase 4K */
    case 0x52:                          /* block erase 32K */
    case 0xd8:                          /* block erase 64K */
    case 0x60:                          /* chip erase */
    case 0xc7: {
        uint32_t addr = 0;

        for (i = 0; i < 3 && i < nargs; i++) {
            addr = (addr << 8) | args[i];
        }
        switch (mosi[0]) {
        case 0x02:
            mt7621_flash_write_bytes(s, addr, mosi + 4, n - 4);
            break;
        case 0x20:
            mt7621_flash_erase(s, addr, 4096);
            break;
        case 0x52:
            mt7621_flash_erase(s, addr, 32768);
            break;
        case 0xd8:
            mt7621_flash_erase(s, addr, 65536);
            break;
        case 0x60:
        case 0xc7:
            if (s->flash) {
                memset(s->flash, 0xff, s->flash_size);
            }
            break;
        default:
            break;
        }
        s->spi_read_mode = false;
        break;
    }
    default:
        s->spi_read_mode = false;
        break;
    }

    /* RX bytes were already produced above for the read commands; for the
     * others, RX captures 0xff. */
    (void)pos;
    (void)rx_pos;
}

static void mt7621_spi_exec(MT7621State *s)
{
    uint32_t trans = s->spi_regs[SPI_TRANS / 4];
    uint32_t morebuf = s->spi_regs[SPI_MOREBUF / 4];
    uint32_t opcode = s->spi_regs[SPI_OPCODE / 4];
    uint8_t mosi[80];
    uint8_t miso[32];
    unsigned cmd_len, mosi_len, rx_len;
    uint32_t cmd_bits = (morebuf >> 24) & 0x3f;
    uint32_t miso_bits = (morebuf >> 12) & 0x1ff;
    uint32_t mosi_bits = morebuf & 0x1ff;
    int i;

    if (morebuf) {
        cmd_len = cmd_bits / 8;
        mosi_len = mosi_bits / 8;
        rx_len = miso_bits / 8;
    } else {
        /*
         * Legacy (non MOREBUF) mode: TRANS holds the MOSI byte count in
         * bits 3:0 and the MISO byte count in bits 7:4.  RouterBOOT drives
         * the block this way and packs the command word as
         *
         *      opcode            in bits  7:0
         *      address byte 23:16 in bits 31:24
         *      address byte 15:8  in bits 23:16
         *      address byte  7:0  in bits 15:8
         *
         * so the wire order is opcode first, then the address MSB first.
         */
        cmd_len = trans & 0xf;
        mosi_len = 0;
        rx_len = (trans >> 4) & 0xf;
    }
    if (cmd_len > 4) {
        cmd_len = 4;
    }
    if (getenv("MT7621_SPISTAT")) {
        static unsigned long long tx;
        static uint32_t low, high;
        uint32_t a = ((opcode >> 24) << 16) | ((opcode >> 8) & 0xffff);

        tx++;
        if (cmd_len >= 4 && (opcode & 0xff) == 0x03) {
            if (!low || a < low) {
                low = a;
            }
            if (a > high) {
                high = a;
            }
        }
        if (!(tx % 50000)) {
            fprintf(stderr, "spi: %llu tx, last op=%08x addr=%06x, "
                    "read window %06x..%06x\n",
                    tx, opcode, a, low, high);
        }
    }
    if (cmd_len + mosi_len > sizeof(mosi)) {
        mosi_len = sizeof(mosi) - cmd_len;
    }
    if (rx_len > sizeof(s->spi_rx)) {
        rx_len = sizeof(s->spi_rx);
    }

    if (getenv("MT7621_SPILOG")) {
        fprintf(stderr, "spi: exec trans=%08x more=%08x op=%08x "
                "cmd_len=%u mosi_len=%u rx_len=%u\n",
                trans, morebuf, opcode, cmd_len, mosi_len, rx_len);
    }

    if (morebuf) {
        /*
         * MOREBUF command bytes are packed in the OPCODE register with the
         * first byte on the wire in the *low* byte, exactly like the legacy
         * TRANS path below.  RouterOS 6.36 issues its status read as
         * cmd_bits=32 with OPCODE=0x00000005 (RDSR); building this MSB-first
         * put 0x00 on the wire, the model fell through to "unknown opcode"
         * and returned 0xff, which stalled the yaffs mount with
         * "raspi_wait_ready: read_sr fail: ff".
         */
        for (i = 0; i < (int)cmd_len; i++) {
            mosi[i] = (opcode >> (8 * i)) & 0xff;
        }
    } else {
        static const int rot[4] = { 0, 24, 16, 8 };
        for (i = 0; i < (int)cmd_len; i++) {
            mosi[i] = (opcode >> rot[i]) & 0xff;
        }
    }
    for (i = 0; i < (int)mosi_len; i++) {
        mosi[cmd_len + i] = ((const uint8_t *)&s->spi_regs[SPI_DATA0 / 4])[i];
    }

    if (mt7621_trace) {
        QEMU_BUILD_BUG_ON(0);
    }

    memset(miso, 0xff, sizeof(miso));
    if (cmd_len == 0 && rx_len == 0 && mosi_len == 0) {
        /* nothing to do */
    } else if (cmd_len == 0) {
        if (s->spi_read_mode) {
            for (i = 0; i < (int)rx_len; i++) {
                miso[i] = mt7621_flash_read_byte(s, s->spi_addr++);
            }
        }
    } else {
        mt7621_spi_flash_run(s, mosi, cmd_len + mosi_len, cmd_len,
                             miso, rx_len);
    }

    for (i = 0; i < (int)rx_len; i++) {
        ((uint8_t *)&s->spi_regs[SPI_DATA0 / 4])[i] = miso[i];
    }

    trans &= ~(SPI_TRANS_START | SPI_TRANS_BUSY);
    s->spi_regs[SPI_TRANS / 4] = trans;
}

static uint64_t mt7621_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xff;
    uint32_t val;

    if (off >= 0x04 && off < 0x24) {
        /* FIFO: expose the individual bytes written by the driver. */
        val = ((const uint32_t *)s->spi_regs)[off / 4];
    } else {
        val = s->spi_regs[off / 4];
    }
    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: spi read  %03x -> %08x\n",
                      (unsigned)off, val);
    }
    return val;
}

static void mt7621_spi_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xff;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: spi write %03x <- %08x\n",
                      (unsigned)off, (uint32_t)val);
    }
    if (getenv("MT7621_SPILOG") &&
        (off <= SPI_DATA0 + 0x18 || off == SPI_MASTER ||
         off == SPI_MOREBUF || off == SPI_POLAR)) {
        fprintf(stderr, "spi: reg %03x <- %08x\n",
                (unsigned)off, (uint32_t)val);
    }
    if (off < 0x04) {
        s->spi_regs[off / 4] = val;
        if (val & SPI_TRANS_START) {
            mt7621_spi_exec(s);
        }
        return;
    }
    if (off >= 0x04 && off < 0x24) {
        ((uint32_t *)s->spi_regs)[off / 4] = val;
        return;
    }
    s->spi_regs[off / 4] = val;
    if (off == SPI_POLAR && val == 0) {
        /* Chip select deasserted: the next read starts a new command. */
        s->spi_read_mode = false;
    }
}

static const MemoryRegionOps mt7621_spi_ops = {
    .read = mt7621_spi_read,
    .write = mt7621_spi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt7621_nfi_cmd(MT7621State *s, uint32_t cmd)
{
    /*
     * Samsung K9F1G08U0B: 1 Gbit, 2 KiB pages, 64 B spare, 128 KiB blocks.
     * Both Breed and Linux know this part; Breed also derives the same
     * geometry from byte 3 when its table only matches on byte 1.
     * Byte 3 is the legacy geometry code: bits 1:0 scale the page size from
     * 1 KiB (so 1 = 2 KiB), bit 2 doubles the spare area (8 -> 16 bytes per
     * 512-byte sector, i.e. 64 bytes per page) and bits 5:4 scale the block
     * size from 64 KiB (so 1 = 128 KiB).
     */
    static const uint8_t nand_id[8] = {
        0xec, 0xf1, 0x00, 0x15, 0x40, 0x00, 0x00, 0x00
    };
    uint8_t status = 0xe0;

    if (getenv("MT7621_NFILOG")) {
        fprintf(stderr, "nfi: cmd %02x col %x row %x\n", cmd, s->nfi_col, s->nfi_row);
    }
    s->nfi_served = 0;
    s->nfi_last_cmd = cmd;
    /*
     * Only commands that begin a new operation restart the address byte
     * stream; program/erase confirm (0x10/0xd0) and read start (0x30)
     * must still see the address they were given.
     */
    switch (cmd) {
    case 0x00:
    case 0x60:
    case 0x80:
    case 0x90:
    case 0xff:
        s->nfi_addr_len = 0;
        break;
    default:
        break;
    }

    switch (cmd) {
    case 0x90:                          /* read id */
        mt7621_nfi_fifo_put(s, nand_id, sizeof(nand_id));
        break;
    case 0x30:                          /* read: page data + spare */
        mt7621_nfi_page_read(s);
        break;
    case 0x70:                          /* read status */
        mt7621_nfi_fifo_put(s, &status, 1);
        break;
    case 0x80:                          /* page program */
        s->nfi_wbuf_len = 0;
        s->nfi_writing = true;
        break;
    case 0x10:                          /* program confirm */
        if (s->nfi_writing && s->nand) {
            uint64_t off = mt7621_nfi_addr_row(s) * s->nand_stride;
            uint32_t n = s->nfi_wbuf_len;

            if (n > s->nand_stride) {
                n = s->nand_stride;
            }
            if (off + n <= s->nand_size) {
                memcpy(s->nand + off, s->nfi_wbuf, n);
                mt7621_nand_persist(s, off, s->nfi_wbuf, n);
            }
        }
        s->nfi_writing = false;
        {
            uint8_t st = 0xe0;
            mt7621_nfi_fifo_put(s, &st, 1);
        }
        break;
    case 0xd0:                          /* erase confirm */
        if (s->nand) {
            uint64_t row = mt7621_nfi_addr_row(s);
            uint64_t blk_start = (row / MT7621_NAND_PAGES_BLK) *
                                 MT7621_NAND_PAGES_BLK * s->nand_stride;
            uint64_t blk_size = (uint64_t)MT7621_NAND_PAGES_BLK *
                                s->nand_stride;

            if (blk_start + blk_size <= s->nand_size) {
                memset(s->nand + blk_start, 0xff, blk_size);
                mt7621_nand_persist(s, blk_start, s->nand + blk_start,
                                    blk_size);
            }
        }
        {
            uint8_t st = 0xe0;
            mt7621_nfi_fifo_put(s, &st, 1);
        }
        break;
    case 0x00:                          /* read setup / reset */
    case 0xff:
        s->nfi_fifo_len = 0;
        s->nfi_fifo_pos = 0;
        break;
    default:
        break;
    }
}

static uint64_t mt7621_nfi_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xfff;
    uint32_t word = s->nfi_regs[off >> 2];
    uint64_t val;

    /*
     * ECC block (0x800+): the emulated flash is error free, so report every
     * engine as idle with zero corrected/uncorrectable bits.  Writing the
     * control registers has no effect that the firmware can observe.
     */
    if (off >= 0x800) {
        /*
         * 0x80c / 0x90c are the encoder and decoder status words; bit 0
         * reports "engine idle", so it has to read set.  Everything else
         * (corrected / uncorrectable counts) is zero for clean data.
         */
        if (off == 0x80c || off == 0x90c) {
            return 1;               /* encoder / decoder idle */
        }
        if (off == 0x918) {
            return 0xffff;          /* decoder done, one bit per sector */
        }
        return 0;                   /* no corrected / uncorrectable bits */
    }

    switch (off) {
    case NFI_DATAR:
    {
        uint32_t v = 0;
        unsigned i;

        /*
         * CNFG_BYTE_RW selects byte mode: the data port then hands out one
         * byte per access, even for word-wide reads.  Page transfers clear
         * it and move `size` bytes per access.
         */
        if (s->nfi_regs[NFI_CNFG >> 2] & (1u << 6)) {
            uint8_t b = 0xff;

            if (s->nfi_fifo_pos < s->nfi_fifo_len) {
                b = s->nfi_fifo[s->nfi_fifo_pos++];
                s->nfi_served++;
            }
            val = b;
            break;
        }

        for (i = 0; i < size; i++) {
            uint8_t b = 0xff;

            if (s->nfi_fifo_pos < s->nfi_fifo_len) {
                b = s->nfi_fifo[s->nfi_fifo_pos++];
                s->nfi_served++;
            }
            v |= (uint32_t)b << (8 * i);
        }
        val = v;
        break;
    }
    case NFI_PIO_DIRDY:
        if (s->nfi_writing) {
            val = 1;            /* always ready to accept write data */
        } else {
            val = (s->nfi_fifo_pos < s->nfi_fifo_len) ? 1 : 0;
        }
        if (getenv("MT7621_NFILOG") && !val) {
            fprintf(stderr, "pio not ready: last cmd %02x col %x row %x addrlen %u\n",
                    s->nfi_last_cmd, s->nfi_col, s->nfi_row, s->nfi_addr_len);
        }
        break;
    case NFI_INTR_STA:
        /* Report everything the firmware enabled as already pending. */
        val = s->nfi_regs[NFI_INTR_EN >> 2];
        break;
    case NFI_STA:
        val = 0;
        break;
    case NFI_ADDRCNTR:
        /* SEC_CNTR is bits 15:12; report all sectors transferred so the
         * driver's program-completion poll succeeds immediately. */
        val = 0x0000f000;
        break;
    case 0xa0 ... 0xbf:
        /*
         * NFI_FDML/FDMM: the spare bytes the controller extracted from the
         * last page.  The dump has no OOB area, so report it as erased --
         * which also means "not a bad block".
         */
        val = 0xffffffff;
        break;
    default:
        switch (size) {
        case 1:
            val = (word >> ((off & 3) * 8)) & 0xff;
            break;
        case 2:
            val = (word >> ((off & 2) * 8)) & 0xffff;
            break;
        default:
            val = word;
            break;
        }
        break;
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: nfi read  %03x (size %u) -> %0*"
                      PRIx64 "\n", (unsigned)off, size, size * 2, val);
    }
    return val;
}

static void mt7621_nfi_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MT7621State *s = opaque;
    uint32_t off = addr & 0xfff;
    uint32_t shift = (off & 3) * 8;
    uint32_t mask;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: nfi write %03x (size %u) <- %0*"
                      PRIx64 "\n", (unsigned)off, size, size * 2, val);
    }

    if (off >= 0x800) {
        return;                 /* ECC control: no visible effect */
    }

    switch (off) {
    case NFI_DATAW:
        if (s->nfi_writing) {
            unsigned i;

            for (i = 0; i < size && s->nfi_wbuf_len < sizeof(s->nfi_wbuf); i++) {
                s->nfi_wbuf[s->nfi_wbuf_len++] =
                    ((uint32_t)val >> (8 * i)) & 0xff;
            }
        }
        return;
    case NFI_CMD:
        s->nfi_regs[off >> 2] = (uint32_t)val;
        mt7621_nfi_cmd(s, (uint32_t)val);
        return;
    case NFI_CON:
        s->nfi_regs[off >> 2] = (uint32_t)val;
        if (val & 0x2) {        /* NFI_RST */
            s->nfi_fifo_len = 0;
            s->nfi_fifo_pos = 0;
            s->nfi_col = 0;
            s->nfi_row = 0;
        }
        return;
    case NFI_ADDRNOB:
        /*
         * Two conventions: Breed programs a cycle count once (e.g. 0x22 =
         * two column + two row cycles) with the values in COLADDR/ROWADDR,
         * while the Linux driver sends every address byte separately as
         * COLADDR followed by ADDRNOB = 1.  Accumulate the byte stream.
         */
        s->nfi_regs[off >> 2] = (uint32_t)val;
        if (val == 1) {
            if (s->nfi_addr_len < ARRAY_SIZE(s->nfi_addr)) {
                s->nfi_addr[s->nfi_addr_len++] = s->nfi_addr_pending & 0xff;
            }
        } else {
            s->nfi_addr_len = 0;
        }
        return;
    case NFI_COLADDR:
        s->nfi_addr_pending = (uint32_t)val;
        s->nfi_col = (uint32_t)val;
        s->nfi_regs[off >> 2] = (uint32_t)val;
        return;
    case NFI_ROWADDR:
        if (getenv("MT7621_NFILOG")) {
            fprintf(stderr, "rowaddr <- %x (size %u)\n", (unsigned)val, size);
        }
        s->nfi_row = (uint32_t)val;
        s->nfi_regs[off >> 2] = (uint32_t)val;
        return;
    default:
        break;
    }

    switch (size) {
    case 1:
        mask = 0xff;
        break;
    case 2:
        shift = (off & 2) * 8;
        mask = 0xffff;
        break;
    default:
        shift = 0;
        mask = 0xffffffff;
        break;
    }
    s->nfi_regs[off >> 2] =
        (s->nfi_regs[off >> 2] & ~(mask << shift)) |
        ((((uint32_t)val) & mask) << shift);
}

static const MemoryRegionOps mt7621_nfi_ops = {
    .read = mt7621_nfi_read,
    .write = mt7621_nfi_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt7621_nand_load(MT7621State *s, const char *filename)
{
    int fd;
    off_t sz;

    if (!filename) {
        return;
    }

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return;
    }

    sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz > 0) {
        s->nand = g_malloc(sz);
        if (pread(fd, s->nand, sz, 0) == sz) {
            s->nand_size = sz;
        } else {
            g_free(s->nand);
            s->nand = NULL;
        }
    }
    close(fd);

    /*
     * Two dump layouts are common:
     *   main area only : pages of 2048 bytes  (e.g. 128 MiB for 128 MiB
     *                    of flash, 65536 pages)
     *   full die dump  : pages of 2048 + 64 spare (132 MiB for the same
     *                    flash); "mtdall" style dumps look like this
     * Pick the one that gives a power-of-two page count, which real NAND
     * always has.
     */
    if (s->nand_size) {
        uint64_t pages = s->nand_size / (MT7621_NAND_PAGE_SIZE +
                                         MT7621_NAND_OOB_SIZE);

        if (s->nand_size % (MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE)
            == 0 && pages && !(pages & (pages - 1))) {
            s->nand_stride = MT7621_NAND_PAGE_SIZE + MT7621_NAND_OOB_SIZE;
        } else {
            s->nand_stride = MT7621_NAND_PAGE_SIZE;
        }
        s->nand_pages = s->nand_size / s->nand_stride;
        if (s->nand_stride && s->nand_stride != MT7621_NAND_PAGE_SIZE) {
            printf("mt7621: NAND dump includes the spare area "
                   "(%u-byte pages, %u pages)\n",
                   s->nand_stride, s->nand_pages);
        }
    }
}

/*
 * SYS_RSTCTL (0x34): Linux's ralink restart handler ends with
 * rt_sysc_w32(RSTCTL_RESET_SYSTEM, SYSC_REG_RESET_CTRL), i.e. a 32-bit
 * write of bit 0.  Anything else (peripheral reset bits, the PCI reset
 * mask the same handler does first) is just latched.
 */
static void mt7621_rstctl_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
    MT7621State *s = opaque;
    uint32_t old = s->sysctl_regs[SYS_RSTCTL / 4];

    s->sysctl_regs[SYS_RSTCTL / 4] = val;

    if ((val & 1) && !(old & 1)) {
        if (getenv("MT7621_RSTLOG") && s->cpu) {
            CPUMIPSState *env = &s->cpu->env;
            uint32_t sp = env->active_tc.gpr[29];
            uint32_t pc = env->active_tc.PC;
            uint32_t ra = env->active_tc.gpr[31];
            int i;

            fprintf(stderr, "restart: pc=%08x ra=%08x sp=%08x\n",
                    pc, ra, sp);
            for (i = 0; i < 64; i++) {
                uint32_t v;
                if (address_space_rw(&address_space_memory,
                                     sp + i * 4, MEMTXATTRS_UNSPECIFIED,
                                     (uint8_t *)&v, 4, false)) {
                    break;
                }
                if (v >= 0x80000000 && v < 0x82000000) {
                    fprintf(stderr, "  stack[%2d] %08x\n", i, v);
                }
            }
        }
        if (getenv("MT7621_NO_RESET")) {
            /* keep the machine frozen so the ring buffer can be read */
            fprintf(stderr, "mt7621: guest requested a reset, holding\n");
            return;
        }
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t mt7621_rstctl_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;

    return s->sysctl_regs[SYS_RSTCTL / 4];
}

static const MemoryRegionOps mt7621_rstctl_ops = {
    .read = mt7621_rstctl_read,
    .write = mt7621_rstctl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps mt7621_reg_ops = {
    .read = mt7621_reg_read,
    .write = mt7621_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * ralink,cevt-systick: 16-bit up-counter at 50 kHz with a compare register.
 * Linux uses it as a oneshot clockevent, so the only event we need to model
 * is "counter reached COMPARE".
 */
static uint32_t mt7621_systick_count(MT7621State *s, int64_t now)
{
    int64_t elapsed;

    if (!(s->systick_regs[SYSTICK_CONFIG / 4] & SYSTICK_CFG_CNT_EN)) {
        return s->systick_base_count & 0xffff;
    }

    elapsed = now - s->systick_base_ns;
    if (elapsed < 0) {
        elapsed = 0;
    }
    return (s->systick_base_count +
            (uint32_t)(elapsed * MT7621_SYSTICK_HZ / NANOSECONDS_PER_SECOND))
           & 0xffff;
}

static void mt7621_systick_set_pending(MT7621State *s, bool pending)
{
    s->systick_pending = pending;
    if (getenv("MT7621_NO_SYSTICK")) {
        return;                     /* bring-up probe: no GIC input at all */
    }
    qemu_set_irq(s->systick_irq, pending);
}

static void mt7621_systick_rearm(MT7621State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t count, compare, delta;

    timer_del(s->systick_timer);
    if (!(s->systick_regs[SYSTICK_CONFIG / 4] & SYSTICK_CFG_CNT_EN)) {
        return;
    }

    count = mt7621_systick_count(s, now);
    compare = s->systick_regs[SYSTICK_COMPARE / 4] & 0xffff;
    delta = (compare - count) & 0xffff;
    if (!delta) {
        delta = 0x10000;
    }

    /* Re-anchor so the counter keeps its current value. */
    s->systick_base_count = count;
    s->systick_base_ns = now;
    timer_mod(s->systick_timer,
              now + (int64_t)delta * NANOSECONDS_PER_SECOND / MT7621_SYSTICK_HZ);
}

/*
 * The 16-bit counter free-runs and matches COMPARE once per wrap, so the
 * device keeps ticking whether or not the guest re-arms it.  The interrupt
 * is asserted as a short pulse: the real one stays up until the handler
 * writes COMPARE, but a pulse keeps a guest that never acknowledges it
 * from wedging the CPU in the ISR.
 */
static void mt7621_systick_expire(void *opaque)
{
    MT7621State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->systick_base_count = s->systick_regs[SYSTICK_COMPARE / 4] & 0xffff;
    s->systick_base_ns = now;
    mt7621_systick_set_pending(s, true);
    timer_mod(s->systick_pulse_off, now + 20000);
    /* Keep a steady tick for firmware that arms COMPARE only once. */
    s->systick_regs[SYSTICK_COMPARE / 4] =
        (s->systick_base_count + MT7621_SYSTICK_PERIOD) & 0xffff;
    mt7621_systick_rearm(s);
}

static void mt7621_systick_pulse_release(void *opaque)
{
    MT7621State *s = opaque;

    mt7621_systick_set_pending(s, false);
}

static uint64_t mt7621_systick_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t val;

    switch (addr & 0xf) {
    case SYSTICK_COUNT:
        val = mt7621_systick_count(s, now);
        break;
    default:
        val = s->systick_regs[(addr & 0xf) >> 2];
        break;
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: systick read %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    return val;
}

static void mt7621_systick_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    MT7621State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: systick write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }

    switch (addr & 0xf) {
    case SYSTICK_CONFIG:
        s->systick_base_count = mt7621_systick_count(s, now);
        s->systick_base_ns = now;
        s->systick_regs[SYSTICK_CONFIG / 4] = val;
        if (!(val & SYSTICK_CFG_CNT_EN)) {
            timer_del(s->systick_timer);
            mt7621_systick_set_pending(s, false);
        } else {
            mt7621_systick_rearm(s);
        }
        break;
    case SYSTICK_COMPARE:
        s->systick_regs[SYSTICK_COMPARE / 4] = val;
        /* The compare write both clears the match and arms the next one. */
        mt7621_systick_set_pending(s, false);
        mt7621_systick_rearm(s);
        break;
    default:
        s->systick_regs[(addr & 0xf) >> 2] = val;
        break;
    }
}

static const MemoryRegionOps mt7621_systick_ops = {
    .read = mt7621_systick_read,
    .write = mt7621_systick_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* The CM/CPC block is 64 KiB, so it needs a wider index than mt7621_reg_ops. */
static uint64_t mt7621_cm_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621State *s = opaque;
    uint32_t *regs = s->cm_regs;
    uint32_t off = addr & (MT7621_CM_SIZE - 1);
    uint32_t val;

    /*
     * COCB+0x8 is aliased, and both aliases are polled during bring-up:
     *
     *   0x4008  CPC_CO_STAT_CONF - what Linux polls; CPC base is 0x1fbf0000
     *   0xc008  GCR_CO_COHERENCE - what RouterBOOT polls (GCR at 0x1fbf8000)
     *
     * Report the core as coherent (bit 0) and in sequence state U6
     * (bits 22:19 == 7), the "coherent execution" state cps_smp_setup() /
     * boot_core() waits for before it considers a secondary core started.
     */
    if (off == 0x4008 || off == MT7621_GCR_OFF + 0x4008) {
        val = 0x00380001;
    } else if (off == MT7621_GCR_OFF + 0x2028) {
        /*
         * GCR_CL_ID: the core-local block is per-core, so the reader sees
         * its own id.  mips_cps_core_entry uses this to find its entry in
         * mips_cps_core_bootcfg[]; returning 0 for everyone made every
         * secondary jump through cpu 0's (empty) VPE config, i.e. to PC 0.
         */
        val = current_cpu ? (uint32_t)current_cpu->cpu_index : 0;
    } else {
        val = regs[off >> 2];
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: cm read  %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    /*
     * MT7621_CMLOG traces the registers the firmware/kernel actually use.
     * RouterBOOT takes a catastrophic path as soon as GCR_GIC_STATUS.EX is
     * set, so this tells us who read it and from where.
     */
    if (mt7621_flag("MT7621_CMLOG") &&
        (off == MT7621_GCR_OFF + GCR_GIC_STATUS ||
         off == MT7621_GCR_OFF + GCR_GIC_BASE ||
         off == MT7621_GCR_OFF + GCR_CONFIG)) {
        fprintf(stderr, "mt7621: cm read %04x pc=%08x -> %08x\n", off,
                s->cpu ? (uint32_t)s->cpu->env.active_tc.PC : 0, val);
    }
    return val;
}

/*
 * Start a secondary core the way the CM/CPC pair does it: CPU reset, then
 * fetch from GCR_Cx_RESET_BASE (the kernel puts CKSEG1ADDR(mips_cps_core_entry)
 * there) with BEV/ERL cleared, because cps-vec.S expects to come out of reset
 * in a normal privileged state.
 */
static void mt7621_start_core_work(CPUState *cs, run_on_cpu_data data)
{
    MT7621State *s = data.host_ptr;
    CPUMIPSState *env = cpu_env(cs);

    cpu_reset(cs);
    env->active_tc.PC = s->co_reset_base & 0xfffff000u;  /* keep KSEG1 */
    env->CP0_Status &= ~((1u << CP0St_BEV) | (1u << CP0St_ERL));
    /*
     * Mark the VPE enabled & activated, like firmware does on the real
     * 1004Kc.  mips_cpu_has_work() forces "no work" when mips_vpe_active()
     * is false, so without VPEConf0.VPA a halted secondary can never wake
     * on an interrupt (CPU1 sat in r4k_wait with IP7+IP2 pending forever).
     */
    env->CP0_VPEConf0 |= (1u << CP0VPEC0_VPA);
    env->active_tc.CP0_TCStatus |= (1u << CP0TCSt_A);
    env->active_tc.CP0_TCHalt &= ~1u;
    if (env->mvp) {
        env->mvp->CP0_MVPControl |= (1u << CP0MVPCo_EVP);
    }
    cs->halted = 0;
}

static void mt7621_start_core(MT7621State *s)
{
    /*
     * CM 2.x semantics: GCR_CL_OTHER (0xa018) selects the core for the
     * COCB redirect, CPC_CL_OTHER (0x2010) selects it for the CPC.  The
     * kernel writes both (mips_cm_lock_other / mips_cpc_lock_other); the
     * CPC one is the authoritative target for CPC_Cx_CMD_RESET.
     */
    unsigned core = (s->cpc_other >> 16) & 0xff;
    CPUState *cs;

    if (!core) {
        core = (s->co_other >> 16) & 0x1f;
    }
    if (core == 0 || core >= s->ncpus) {
        return;
    }
    if (mt7621_flag("MT7621_SMPLOG")) {
        fprintf(stderr, "mt7621: start core %u at %08x (cpc_other=%08x)\n",
                core, s->co_reset_base & 0xfffff000u, s->cpc_other);
    }
    cs = CPU(s->cpus[core]);
    /*
     * The target vCPU is halted; run the reset on its own thread so the
     * halt is cleared and the thread is kicked out of its wait.
     */
    async_safe_run_on_cpu(cs, mt7621_start_core_work,
                          RUN_ON_CPU_HOST_PTR(s));
}

static void mt7621_cpu_reset(void *opaque)
{
    MIPSCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);

    cpu_reset(cs);
    if (cs->cpu_index != 0) {
        /* Secondary cores wait for the CPC to release them. */
        cs->halted = 1;
    }
}

static void mt7621_cm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MT7621State *s = opaque;
    uint32_t *regs = s->cm_regs;
    uint32_t off = addr & (MT7621_CM_SIZE - 1);

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: cm write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }
    regs[off >> 2] = val;

    if (mt7621_flag("MT7621_SMPLOG") &&
        ((off >= 0x2000 && off < 0x2040) || (off >= 0x4000 && off < 0x4040) ||
         (off >= 0xa000 && off < 0xa040) || (off >= 0xc000 && off < 0xc040))) {
        fprintf(stderr, "mt7621: cm w %04x <- %08x\n", off, (uint32_t)val);
    }

    /* GCR core-other block (selected core + its reset vector). */
    if (off == MT7621_GCR_OFF + 0x2018) {
        /* GCR_CL_OTHER (CM < 3): core << 16 */
        s->co_other = val;
    } else if (off == 0x2010) {
        /* CPC_CL_OTHER (CM < 3): core << 16 */
        s->cpc_other = val;
    } else if (off == MT7621_GCR_OFF + 0x4018) {
        s->co_other = val;
    } else if (off == MT7621_GCR_OFF + 0x4020) {
        s->co_reset_base = val;
    } else if (off == 0x4000 && (val & 0xf) == 0x4) {
        /* CPC_Cx_CMD_RESET through the core-other redirect block. */
        mt7621_start_core(s);
    }
}

static const MemoryRegionOps mt7621_cm_ops = {
    .read = mt7621_cm_read,
    .write = mt7621_cm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Attach an optional writable NAND image passed as
 *   -drive file=nand.img,if=none,id=nand,format=raw
 * A fresh/empty image is seeded from the dump; an existing one is used as
 * is, so writes survive QEMU restarts while the dump stays pristine.
 */
static void mt7621_nand_attach(MT7621State *s)
{
    const char *path = getenv("MT7621_NAND_IMAGE");
    BlockBackend *blk;
    int64_t len;
    int fd;
    off_t sz;

    s->nand_fd = -1;

    /*
     * Preferred: a plain image file we create and seed ourselves, so the
     * user does not have to know the flash size:
     *   MT7621_NAND_IMAGE=~/nand-state.img qemu-system-mipsel ...
     */
    if (path) {
        if (!s->nand) {
            warn_report("mt7621: MT7621_NAND_IMAGE needs a readable boot dump");
            return;
        }
        fd = open(path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            warn_report("mt7621: cannot open NAND image %s: %s, changes will "
                        "be kept in RAM only", path, strerror(errno));
            return;
        }
        sz = lseek(fd, 0, SEEK_END);
        if (sz < 0) {
            warn_report("mt7621: cannot size %s", path);
            close(fd);
            return;
        }
        if ((uint64_t)sz < s->nand_size) {
            if (mt7621_pwrite_all(fd, s->nand, s->nand_size, 0) < 0) {
                warn_report("mt7621: cannot seed %s", path);
                close(fd);
                return;
            }
            printf("mt7621: NAND image %s seeded from the boot dump "
                   "(%" PRIu64 " bytes)\n", path, s->nand_size);
        } else {
            if (pread(fd, s->nand, s->nand_size, 0) != (ssize_t)s->nand_size) {
                warn_report("mt7621: cannot read %s", path);
                close(fd);
                return;
            }
            printf("mt7621: using persistent NAND image %s\n", path);
        }
        s->nand_fd = fd;
        return;
    }

    /* Alternative, QEMU style (the file has to exist):
     *   -drive file=nand.img,if=none,id=nand,format=raw
     */
    blk = blk_by_name("nand");
    if (!blk) {
        return;
    }
    if (!s->nand) {
        warn_report("mt7621: -drive id=nand needs a readable boot dump");
        return;
    }

    len = blk_getlength(blk);
    if (len < 0) {
        warn_report("mt7621: cannot size the NAND image, ignoring it");
        return;
    }
    if (len < s->nand_size) {
        if (blk_pwrite(blk, 0, s->nand_size, s->nand, 0) < 0) {
            warn_report("mt7621: cannot seed the NAND image, changes will "
                        "be kept in RAM only");
            return;
        }
        printf("mt7621: NAND image seeded from the boot dump (%" PRIu64
               " bytes)\n", s->nand_size);
    } else {
        if (blk_pread(blk, 0, s->nand_size, s->nand, 0) < 0) {
            warn_report("mt7621: cannot read the NAND image, changes will "
                        "be kept in RAM only");
            return;
        }
        printf("mt7621: using the persistent NAND image (%" PRIu64
               " bytes)\n", s->nand_size);
    }
    s->nand_blk = blk;
}

/*
 * Read bytes of the flash's *logical* address space (the concatenation of
 * the main areas).  Dumps that include the spare area store 2112 bytes per
 * page, so a linear read would pick up 64 bytes of OOB every 2048 bytes and
 * corrupt anything larger than one page -- which is what a uImage payload
 * is.
 */
static ssize_t mt7621_flash_pread(MT7621State *s, int fd, uint64_t off,
                                  void *buf, size_t len)
{
    uint8_t *dst = buf;
    size_t done = 0;

    while (done < len) {
        uint64_t foff = off + done;
        size_t take = len - done;

        if (s->nand_stride && s->nand_stride != MT7621_NAND_PAGE_SIZE) {
            uint64_t page = foff / MT7621_NAND_PAGE_SIZE;
            uint64_t inpage = foff % MT7621_NAND_PAGE_SIZE;

            if (take > MT7621_NAND_PAGE_SIZE - inpage) {
                take = MT7621_NAND_PAGE_SIZE - inpage;
            }
            foff = page * s->nand_stride + inpage;
        }
        if (pread(fd, dst + done, take, foff) != (ssize_t)take) {
            return -1;
        }
        done += take;
    }
    return len;
}

/*
 * MT7621_FLASH_SAVE=<path> keeps a copy of the (writable) NOR buffer on disk
 * while the guest runs.  The memory window only exposes the first 4 MiB, so
 * pmemsave cannot reach the rest; netinstall writes the whole 16 MiB.
 */
static void mt7621_flash_save_cb(void *opaque)
{
    MT7621State *s = opaque;
    const char *path = getenv("MT7621_FLASH_SAVE");
    int fd;

    if (path && s->flash) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
        if (fd >= 0) {
            if (write(fd, s->flash, s->flash_size) != (ssize_t)s->flash_size) {
                error_report("mt7621: short write saving %s", path);
            }
            close(fd);
        }
    }
    timer_mod(s->flash_save_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
}

/*
 * SPI NOR boards boot from a memory-mapped flash: the BootROM maps the
 * first 4 MiB at 0x1fc00000 and enters at 0xbfc00000.  There is no RAM
 * load and no uImage header; RouterBOOT's own stage-1 header jumps into
 * its second stage at flash offset 0x10000.
 */
static void mt7621_nor_load(MT7621State *s, const char *filename)
{
    int fd;
    off_t sz;
    uint64_t window;

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        error_report("mt7621: cannot open %s: %s", filename, strerror(errno));
        exit(1);
    }
    sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0) {
        error_report("mt7621: %s: empty flash image", filename);
        exit(1);
    }

    s->flash = g_malloc0(sz);
    if (pread(fd, s->flash, sz, 0) != sz) {
        error_report("mt7621: %s: short read", filename);
        exit(1);
    }
    close(fd);
    s->flash_size = sz;
    s->spi_nor = true;

    window = MIN((uint64_t)sz, (uint64_t)MT7621_SPI_WINDOW);
    memory_region_init_ram_ptr(&s->flash_mr, NULL, "mt7621.spi-nor",
                               window, s->flash);
    /*
     * Writable: RouterBOOT does not program settings through the SPI
     * controller at all, it stores them into the memory mapped window
     * ("writing settings to flash.."), which the SoC turns into a flash
     * program cycle.  Keeping the window read-only silently dropped every
     * setting the booter ever saved.
     */
    memory_region_add_subregion(get_system_memory(), MT7621_SPI_FLASH_BASE,
                                &s->flash_mr);

    if (getenv("MT7621_FLASH_SAVE")) {
        s->flash_save_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          mt7621_flash_save_cb, s);
        timer_mod(s->flash_save_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 2000);
    }

    s->entry = MT7621_SPI_FLASH_BASE | 0xa0000000;   /* KSEG1 0xbfc00000 */
    printf("mt7621: SPI NOR %0.1f MiB mapped at 0x%08x, entry 0x%08x\n",
           (double)sz / (1024 * 1024), MT7621_SPI_FLASH_BASE, s->entry);
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
    if (s->nand_stride && s->nand_stride != MT7621_NAND_PAGE_SIZE) {
        total = (total / s->nand_stride) * MT7621_NAND_PAGE_SIZE;
    }

    if (pread(fd, hdr, sizeof(hdr), 0) != sizeof(hdr)) {
        error_report("mt7621: %s: short read", filename);
        exit(1);
    }

    if (ldl_be_p(hdr) == 0x7f454c46) {
        /*
         * ELF image: exactly what RouterBOOT hands over on a netboot.  Load
         * each PT_LOAD segment at its physical address and start at e_entry,
         * which is how the bootloader transfers control.
         */
        uint8_t eh[52];
        uint32_t phoff, phentsize, phnum, elf_entry;
        uint32_t i;

        s->elf_kernel = true;

        if (pread(fd, eh, sizeof(eh), 16) != sizeof(eh)) {
            error_report("mt7621: %s: short ELF header", filename);
            exit(1);
        }
        elf_entry = ldl_le_p(eh + 8);
        phoff     = ldl_le_p(eh + 12);
        phentsize = lduw_le_p(eh + 26);
        phnum     = lduw_le_p(eh + 28);

        if (!s->entry) {
            s->entry = elf_entry;
        }
        for (i = 0; i < phnum; i++) {
            uint8_t ph[32];
            uint32_t type, p_off, paddr, filesz, memsz;
            uint8_t *seg;

            if (pread(fd, ph, sizeof(ph), phoff + i * phentsize) != sizeof(ph)) {
                break;
            }
            type   = ldl_le_p(ph);
            p_off  = ldl_le_p(ph + 4);
            paddr  = ldl_le_p(ph + 12);
            filesz = ldl_le_p(ph + 16);
            memsz  = ldl_le_p(ph + 20);
            if (type != 1) {            /* PT_LOAD */
                continue;
            }
            if ((paddr & 0x1fffffff) + memsz > s->ram.size) {
                error_report("mt7621: %s: segment 0x%x+0x%x does not fit",
                             filename, paddr, memsz);
                exit(1);
            }
            seg = g_malloc0(filesz ? filesz : 1);
            if (filesz && pread(fd, seg, filesz, p_off) != filesz) {
                error_report("mt7621: %s: short segment read", filename);
                exit(1);
            }
            address_space_write(&address_space_memory,
                                paddr & 0x1fffffff, MEMTXATTRS_UNSPECIFIED,
                                seg, filesz);
            if (memsz > filesz) {
                uint8_t *z = g_malloc0(memsz - filesz);

                if (getenv("MT7621_ELF_DIRTYBSS")) {
                    /* mimic a bootloader that does not zero memsz-filesz */
                    memset(z, 0xa5, memsz - filesz);
                }
                address_space_write(&address_space_memory,
                                    (paddr & 0x1fffffff) + filesz,
                                    MEMTXATTRS_UNSPECIFIED, z,
                                    memsz - filesz);
                g_free(z);
            }
            g_free(seg);
            printf("mt7621: ELF segment 0x%08x-0x%08x loaded at 0x%08x\n",
                   p_off, p_off + filesz, paddr);
        }
        /*
         * Bring-up hook: reproduce the argc/argv hand-over RouterBOOT uses,
         * so the netboot path can be compared against the direct one.
         */
        if (getenv("MT7621_ELF_ARGS")) {
            gchar **tok = g_strsplit(getenv("MT7621_ELF_ARGS"), " ", 0);
            uint32_t strp = 0x01800000, argvp = 0x01801000;
            uint32_t ptrs[64];
            int n = 0, ti;

            for (ti = 0; tok[ti] && n < 64; ti++) {
                size_t l = strlen(tok[ti]) + 1;
                address_space_write(&address_space_memory, strp,
                                    MEMTXATTRS_UNSPECIFIED, tok[ti], l);
                ptrs[n++] = 0xa0000000 | strp;
                strp += (l + 3) & ~3;
            }
            address_space_write(&address_space_memory, argvp,
                                MEMTXATTRS_UNSPECIFIED, ptrs, n * 4);
            s->elf_argc = n;
            s->elf_argv = 0xa0000000 | argvp;
            printf("mt7621: ELF args argc=%d argv=%08x\n", n, s->elf_argv);
            g_strfreev(tok);
        }
        printf("mt7621: ELF entry 0x%08x\n", s->entry);
        close(fd);
        return;
    }

    if (s->uimage || ldl_be_p(hdr) == 0x27051956) {
        if (ldl_be_p(hdr) != 0x27051956) {
            error_report("mt7621: %s: not a uImage", filename);
            exit(1);
        }
        uint32_t img_size = (uint32_t)ldl_be_p(hdr + 12);
        /*
         * MT7621 BootROM NAND header: the uImage name field (32 bytes) is
         * overlaid by nand_header_t, so ih_bootloader_offset lives at +52.
         * When non-zero the BootROM first runs the stage-1 DDR init image
         * and then loads the bootloader from that file offset.
         */
        uint32_t bl_off = ldl_be_p(hdr + 52);

        size  = img_size;
        load  = (uint32_t)ldl_be_p(hdr + 16);
        entry = (uint32_t)ldl_be_p(hdr + 20);
        off   = 64;
        if (s->load_addr) {
            load = s->load_addr;
        }
        if (s->entry) {
            entry = s->entry;
        }
        if (bl_off && !getenv("MT7621_STAGE1")) {
            if (bl_off >= 64 + (uint64_t)img_size) {
                error_report("mt7621: bootloader offset 0x%x out of range",
                             bl_off);
                exit(1);
            }
            /*
             * BootROM step 2: the bootloader is loaded to ih_load (a KSEG1
             * address, called out in the MT7621 NAND image header) and
             * entered there.  Stage-1 has already trained DDR by now.
             */
            load  = (uint32_t)ldl_be_p(hdr + 16);
            entry = (uint32_t)ldl_be_p(hdr + 20);
            off   = bl_off;
            size  = 64 + (uint64_t)img_size - bl_off;
            printf("mt7621: BootROM boot step 2: bootloader 0x%" PRIx64
                   " bytes from file 0x%x -> 0x%" PRIx64 "\n",
                   size, bl_off, load);
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
        ssize_t n = mt7621_flash_pread(s, fd, off, buf, chunk);

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

/*
 * The MT7621 uses the legacy MIPS GIC, and its reset mapping sends every
 * shared interrupt to VP0 pin 0, i.e. CPU interrupt line 2.  That is why
 * Breed only programs SMASK/POL and then enables IM2: it never writes the
 * mapping registers.  QEMU's GIC model resets everything unmapped, so put
 * the SoC default back after the device reset has run.
 */
static void mt7621_gic_default_map(MT7621State *s)
{
    MIPSGICState *gic;
    int i;

    if (!s->gic) {
        return;
    }

    gic = MIPS_GIC(s->gic);
    for (i = 0; i < gic->num_irq; i++) {
        gic->irq_state[i].map_pin = 0;
        gic->irq_state[i].map_vp = 0;
    }
}

static void mt7621_gic_reset(void *opaque)
{
    mt7621_gic_default_map(opaque);
}

static void mt7621_sysctl_defaults(MT7621State *s)
{
    memset(s->sysctl_regs, 0, sizeof(s->sysctl_regs));
    /*
     * "MT76" / "21  ".  Linux's ralink SoC probe compares these two words
     * and panics with "mt7621: unknown SoC" if they do not match.
     */
    s->sysctl_regs[SYS_CHIP_NAME0 / 4]   = 0x3637544d;
    s->sysctl_regs[SYS_CHIP_NAME1 / 4]   = 0x20203132;
    /*
     * SYS_CHIP_REV_ID, decoded the way U-Boot and Breed do it:
     * bit 17 = CPU_ID (second core present), bit 16 = PKG_ID, bits 11:8
     * = version, bits 3:0 = eco.  A real MT7621A has both bits set.
     */
    s->sysctl_regs[SYS_CHIP_REV_ID / 4]  = 0x00030000 | (1 << 8) | 0;
    /*
     * SYSCFG0: CHIP_MODE[3:0] tells the firmware which boot flash is wired
     * up; 0 means "no NAND" and makes board_init skip the NFI entirely.
     * Bits above it: DRAM type and xtal mode.
     */
    s->sysctl_regs[SYS_SYSCFG0 / 4]      = 0x00000180 | s->chip_mode;
    /*
     * CPLL clock config.  RouterBOOT programs 0x40800000 on the RB750Gr3,
     * which is how Linux ends up reporting the 875 MHz CPU clock (and the
     * 1.75 GHz GIC counter derived from it); the cold-reset value 0x40000000
     * makes it report 25 MHz instead, which then disagrees with the emulated
     * timers by a factor of 70.
     */
    s->sysctl_regs[SYS_CLKCFG0 / 4]      = 0x40800000; /* 880 MHz PLL */
    s->sysctl_regs[SYS_CUR_CLK_STS / 4]  = 0x00000101; /* /1 * 1 */
    /*
     * 0x3fc is polled for bit 10 before Breed's DDR3 init: it reports the
     * DDR/PLL calibration as finished. Our DDR is always ready.
     */
    s->sysctl_regs[0x3fc / 4]            = 0x00000400;
    /*
     * SYSC_RSTSTAT reports the reset cause.  Keep the bit we recorded when
     * the watchdog barked (Breed overwrites this register itself once it
     * runs, but a real SoC latches the cause across a watchdog reset).
     */
    if (s->wdt_rst_cause) {
        s->sysctl_regs[SYS_RSTSTAT / 4] = WDT_RST_CAUSE;
        s->wdt_rst_cause = false;
    }
}

static void mt7621_cm_defaults(MT7621State *s)
{
    memset(s->cm_regs, 0, sizeof(s->cm_regs));
    /* GCR_CONFIG.PCORES is "cores minus one". */
    s->cm_regs[(MT7621_GCR_OFF + GCR_CONFIG) / 4] =
        (s->ncpus > 1 ? s->ncpus - 1 : 0);
    s->cm_regs[(MT7621_GCR_OFF + GCR_BASE) / 4]   = 0x1fbf8000;
    s->cm_regs[(MT7621_GCR_OFF + GCR_REV) / 4]    = 0x00000002;
    /*
     * The MT7621 GIC is the legacy MIPS GIC at 0x1fbc0000, not a CM-managed
     * GIC; advertising one in the GCR makes Breed take a CM-GIC path that
     * does not exist on this SoC and crash.
     */
    s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_STATUS) / 4] = 0x00000000;
    s->cm_regs[(MT7621_GCR_OFF + GCR_CPC_STATUS) / 4] = 0x00000001;

    /*
     * MT7621_CMGIC advertises the (legacy) GIC through the CM, which is
     * what Linux 5.6's mips_cps_init() insists on before it registers its
     * SMP ops: without CM_GCR_GIC_STATUS.EX it prints "MIPS CPS SMP unable
     * to proceed without a GIC", leaves mp_ops NULL and dies on the first
     * indirect call through them.  Off by default because RouterBOOT itself
     * jumps through a bogus pointer as soon as the GIC is advertised.
     */
    if (mt7621_flag("MT7621_CMGIC")) {
        s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_STATUS) / 4] = 0x00000001;
        s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_BASE) / 4] =
            (MT7621_GIC_BASE & 0x3fff8000) | 0x1;
    }
    /*
     * MT7621_UP=1 reports a single core (no secondary bring-up).  PCORES is
     * "cores minus one" (mips_cps_numcores() adds one back), so a single
     * core is 0, not 1.
     */
    if (mt7621_flag("MT7621_UP")) {
        s->cm_regs[(MT7621_GCR_OFF + GCR_CONFIG) / 4] = 0x00000000;
    }
}

static void mt7621_dramc_defaults(MT7621State *s)
{
    memset(s->dramc_regs, 0, sizeof(s->dramc_regs));
    s->dramc_regs[0x604 / 4] = 0x00000000;
    s->dramc_regs[0x618 / 4] = 0x00000190;
    s->dramc_regs[0x648 / 4] = 0x03800000;
    s->dramc_regs[0x3fc / 4] = 0x00000400;
}

/*
 * `poweroff` and `halt` on this SoC both end in machine_hang(): interrupts
 * are disabled, all of them are masked and the CPU parks in WAIT.  Nothing
 * is written to the system controller, so the only way to notice is that a
 * CPU which can no longer be woken stays halted.
 */
static void mt7621_poweroff_poll(void *opaque)
{
    MT7621State *s = opaque;
    CPUState *cs = CPU(s->cpu);
    CPUMIPSState *env = &s->cpu->env;
    bool parked = cs->halted && !(env->CP0_Status & (1 << CP0St_IE));

    /*
     * Bring-up: report the CP0/TLB state the guest starts the kernel with.
     * The netboot path hands over from a bootloader that has been running for
     * seconds, so the state it leaves behind matters.
     */
    if (getenv("MT7621_ENTRYLOG") && s->cpu) {
        uint32_t hpc = env->active_tc.PC;

        s->pc_hist[s->pc_hist_idx++ % PC_HIST] = hpc;
        if (!s->rom_logged && hpc >= 0xbfc00000 && hpc < 0xbfc01000) {
            int k;

            s->rom_logged = true;
            fprintf(stderr, "rom reached: pc=%08x status=%08x cause=%08x "
                    "badvaddr=%08x epc=%08x errpc=%08x\n",
                    hpc, env->CP0_Status, env->CP0_Cause,
                    (uint32_t)env->CP0_BadVAddr, (uint32_t)env->CP0_EPC,
                    (uint32_t)env->CP0_ErrorEPC);
            fprintf(stderr, "pcs:");
            for (k = 0; k < PC_HIST; k++) {
                uint32_t v = s->pc_hist[(s->pc_hist_idx + k) % PC_HIST];

                if (v) {
                    fprintf(stderr, " %08x", v);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    if (getenv("MT7621_ENTRYLOG") && !s->entry_logged && s->cpu) {
        uint32_t pc = env->active_tc.PC;

        if (pc >= 0x80b80000 && pc < 0x81100000) {
            int i;

            s->entry_logged = true;
            if (getenv("MT7621_ENTRY_FIX")) {
                int t;

                env->CP0_Status &= ~(1 << CP0St_ERL);
                env->CP0_Config0 = (env->CP0_Config0 & ~7u) | 2;
                for (t = 0; t < MIPS_TLB_MAX; t++) {
                    env->tlb->mmu.r4k.tlb[t].VPN = 0;
                    env->tlb->mmu.r4k.tlb[t].PageMask = 0;
                    env->tlb->mmu.r4k.tlb[t].PFN[0] = 0;
                    env->tlb->mmu.r4k.tlb[t].PFN[1] = 0;
                }
                fprintf(stderr, "entry: normalised status=%08x config0=%08x\n",
                        env->CP0_Status, (uint32_t)env->CP0_Config0);
            }
            fprintf(stderr, "entry: pc=%08x status=%08x cause=%08x wired=%u "
                    "entryhi=%08x index=%u config0=%08x config1=%08x\n",
                    pc, env->CP0_Status, env->CP0_Cause, env->CP0_Wired,
                    (uint32_t)env->CP0_EntryHi, (uint32_t)env->CP0_Index,
                    (uint32_t)env->CP0_Config0, (uint32_t)env->CP0_Config1);
            for (i = 0; i < 8; i++) {
                fprintf(stderr, "  tlb[%d] vpn=%08x mask=%08x asid=%02x "
                        "g=%d pfn0=%08x pfn1=%08x\n", i,
                        env->tlb->mmu.r4k.tlb[i].VPN,
                        env->tlb->mmu.r4k.tlb[i].PageMask,
                        env->tlb->mmu.r4k.tlb[i].ASID,
                        env->tlb->mmu.r4k.tlb[i].G,
                        (uint32_t)env->tlb->mmu.r4k.tlb[i].PFN[0],
                        (uint32_t)env->tlb->mmu.r4k.tlb[i].PFN[1]);
            }
        }
    }
    if (!s->halt_seen_running) {
        if (!cs->halted) {
            s->halt_seen_running = true;
        }
    } else if (parked) {
        if (++s->halt_ticks >= 4) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        }
    } else {
        s->halt_ticks = 0;
    }

    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") ? 1 : 250));
}

/*
 * Kernel command line injection for bring-up.
 *
 * RouterBOOT assembles the kernel command line in a scratch buffer inside
 * its own RAM image and hands a pointer to the kernel, so there is nothing
 * to patch in the flash image.  The buffer only exists for the instant
 * between "loading kernel... OK" and the jump, and its physical address
 * moves with the bootloader build, so sweep the bootloader's RAM window and
 * append the extra options in place.
 *
 * The anchor also appears inside the printk ring buffer (as part of the
 * "Kernel command line: ..." line), so a NUL immediately before it is
 * required to stay on RouterBOOT's copy.
 */
#define MT7621_CMDLINE_BASE         0x01100000
#define MT7621_CMDLINE_SIZE         0x00400000
#define MT7621_CMDLINE_PAGE         0x1000
#define MT7621_CMDLINE_PAGES_TICK   16
#define MT7621_CMDLINE_LIFETIME_NS  (60 * 1000000000LL)

static bool mt7621_cmdline_patch(MT7621State *s, uint32_t phys)
{
    char buf[1024];
    char tmp[1024];
    size_t len, extra = strlen(s->cmdline_extra);
    size_t i;

    if (address_space_read(&address_space_memory, phys,
                           MEMTXATTRS_UNSPECIFIED, buf, sizeof(buf))) {
        return false;
    }
    buf[sizeof(buf) - 1] = 0;
    len = strnlen(buf, sizeof(buf) - 1);
    if (strstr(buf, s->cmdline_extra)) {
        return true;
    }
    if (len + extra + 3 > sizeof(buf)) {
        return false;
    }
    /* Only extend into padding; the buffer may be followed by live data. */
    for (i = len; i <= len + extra + 1; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    snprintf(tmp, sizeof(tmp), "%s %s", buf, s->cmdline_extra);
    if (address_space_write(&address_space_memory, phys,
                            MEMTXATTRS_UNSPECIFIED, tmp, strlen(tmp) + 1)) {
        return false;
    }
    s->cmdline_patches++;
    if (s->cmdline_patches <= 10) {
        fprintf(stderr, "mt7621: cmdline #%u at tick %u: '%s'\n",
                s->cmdline_patches, s->cmdline_ticks, tmp);
    }
    return true;
}

static void mt7621_cmdline_poll(void *opaque)
{
    MT7621State *s = opaque;
    static const char anchor[] = "root=/dev/ram0 bootimage=1";
    uint8_t page[MT7621_CMDLINE_PAGE + 64];
    int n;

    s->cmdline_ticks++;

    if (s->cpu && !s->cmdline_kernel_seen) {
        uint32_t pc = s->cpu->env.active_tc.PC;

        if (pc >= 0x80100000 && pc < 0x81000000) {
            s->cmdline_kernel_seen = true;
            if (getenv("MT7621_TRACE")) {
                fprintf(stderr, "mt7621: kernel PC %08x at tick %u "
                        "(patches %u)\n", pc, s->cmdline_ticks,
                        s->cmdline_patches);
            }
        }
    }

    if (getenv("MT7621_TRACE") && !(s->cmdline_ticks % 10000)) {
        fprintf(stderr, "mt7621: cmdline poll #%u phys=%08x patches=%u\n",
                s->cmdline_ticks, s->cmdline_phys, s->cmdline_patches);
    }

    if (s->cmdline_phys) {
        /*
         * Once the buffer address is known, watch it closely: RouterBOOT
         * writes the string and jumps within a millisecond or so, and the
         * kernel parses it almost immediately after entry.
         */
        int r = address_space_read(&address_space_memory, s->cmdline_phys,
                                   MEMTXATTRS_UNSPECIFIED, page, 1024);
        if (getenv("MT7621_TRACE") && page[0] &&
            !(s->cmdline_ticks % 20000)) {
            fprintf(stderr, "mt7621: cmdline peek #%u r=%d '%c%c%c%c%c%c'\n",
                    s->cmdline_ticks, r, page[0], page[1], page[2],
                    page[3], page[4], page[5]);
        }
        if (!memcmp(page, anchor, sizeof(anchor) - 1)) {
            mt7621_cmdline_patch(s, s->cmdline_phys);
        }
    } else {
        /* Address unknown: sweep the bootloader's RAM window. */
        for (n = 0; n < MT7621_CMDLINE_PAGES_TICK; n++) {
            uint32_t off = s->cmdline_cursor;
            uint32_t phys = MT7621_CMDLINE_BASE + off;
            uint8_t *hit;

            if (address_space_read(&address_space_memory, phys,
                                   MEMTXATTRS_UNSPECIFIED, page,
                                   sizeof(page))) {
                break;
            }
            s->cmdline_cursor += MT7621_CMDLINE_PAGE;
            if (s->cmdline_cursor + sizeof(page) >= MT7621_CMDLINE_SIZE) {
                s->cmdline_cursor = 0;
            }

            hit = memmem(page, MT7621_CMDLINE_PAGE, anchor,
                         sizeof(anchor) - 1);
            if (!hit || hit == page || hit[-1] != 0) {
                continue;
            }
            s->cmdline_phys = phys + (hit - page);
            if (getenv("MT7621_TRACE")) {
                fprintf(stderr, "mt7621: cmdline buffer at phys %08x\n",
                        s->cmdline_phys);
            }
            mt7621_cmdline_patch(s, s->cmdline_phys);
            break;
        }
    }

    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->cmdline_deadline_ns) {
        timer_mod(s->cmdline_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (s->cmdline_phys ? 100000 : 2000000));
    }
}

static void mt7621_phy_defaults(MT7621State *s)
{
    unsigned p, r;

    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    for (p = 0; p < 32; p++) {
        s->phy_regs[p][0]  = 0x0140;    /* BMCR: 1000BASE-T full duplex
                                         * (bit 6 = speed MSB, no autoneg) */
        s->phy_regs[p][2]  = 0x0362;    /* PHY ID1 */
        s->phy_regs[p][3]  = 0x5c30;    /* PHY ID2 */
        s->phy_regs[p][4]  = 0x01e1;    /* 10/100 advertise */
        s->phy_regs[p][9]  = 0x0300;    /* MII_CTRL1000: 1000BASE-T f/h */
    }
    s->phy_link_ns = 0;
    (void)r;
}

static void mt7621_switch_defaults(MT7621State *s)
{
    /*
     * MT7530 identity: CREV (0x7ffc) >> 16 must be 0x7530 and HWTRAP
     * (0x7800) must read non-zero, otherwise mt7530-mdio refuses the chip
     * and DSA never creates the lan/wan ports.
     */
    memset(s->sw_regs, 0, sizeof(s->sw_regs));
    s->sw_page = 0;
    s->sw_r = 0;
    *mt7621_sw_word(s, (0x7ffc >> 6) & 0x3ff, (0x7ffc >> 2) & 0xf) = 0x75300000;
    *mt7621_sw_word(s, (0x7800 >> 6) & 0x3ff, (0x7800 >> 2) & 0xf) = 0x00000001;
}

static void mt7621_reset(void *opaque)
{
    MT7621State *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    if (getenv("MT7621_RSTLOG")) {
        fprintf(stderr, "mt7621: machine reset, cpu pc=%08x ra=%08x\n",
                env->active_tc.PC, env->active_tc.gpr[31]);
    }

    /*
     * The real 1004Kc advertises ULRI + CMGCR, but with them visible this
     * model's OpenWrt guests take the MIPS CPS path and die before printing
     * anything (openwrt 5.15: zero bytes), while RouterOS 3.3.5 is unaffected.
     * Default to the pre-CPS view and let MT7621_CMGCR=1 (or MT7621_CFG3) opt
     * in - MikroTik's 5.6.3 netinstall kernel needs it to find the CM/GIC.
     */
    if (!getenv("MT7621_CMGCR") && !getenv("MT7621_CFG3")) {
        env->CP0_Config3 &= ~((1u << 13) | (1u << 29));
    }
    if (getenv("MT7621_CFG3")) {
        env->CP0_Config3 = strtoul(getenv("MT7621_CFG3"), NULL, 0);
    }

    if (s->wdt_timer) {
        timer_del(s->wdt_timer);
    }

    if (s->cfg1_cache_fields) {
        env->CP0_Config1 = (env->CP0_Config1 & ~0xffffu) |
                           s->cfg1_cache_fields;
    }

    /* Everything the guest programmed is gone after a real reset. */
    memset(s->eth_regs, 0, sizeof(s->eth_regs));
    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    memset(s->spi_regs, 0, sizeof(s->spi_regs));
    s->spi_read_mode = false;
    s->spi_addr = 0;
    s->qtx_dtx = 0;
    s->qtx_base = 0;
    s->qtx_en = false;
    s->rx_drx_idx = 0;
    mt7621_phy_defaults(s);
    s->nfi_fifo_len = 0;
    s->nfi_fifo_pos = 0;
    s->nfi_col = 0;
    s->nfi_row = 0;
    s->nfi_served = 0;
    s->nfi_last_cmd = 0;
    s->nfi_addr_len = 0;
    s->nfi_addr_pending = 0;
    s->nfi_wbuf_len = 0;
    s->nfi_writing = false;
    mt7621_sysctl_defaults(s);
    mt7621_switch_defaults(s);
    mt7621_dramc_defaults(s);
    mt7621_cm_defaults(s);
    memset(s->systick_regs, 0, sizeof(s->systick_regs));

    /*
     * Linux scribbled over the RAM the bootloader lived in, so the BootROM
     * has to load it again before the CPU is released.
     */
    if (s->image_file && (!s->spi_nor || s->elf_kernel)) {
        mt7621_load_image(s, s->image_file);
    }

    /* Malta does the same when a -kernel image is supplied. */
    env->CP0_Status &= ~(1 << 2); /* CP0St_ERL */
    env->active_tc.PC = s->entry;
    if (getenv("MT7621_ENTRY_DIRTY") && s->elf_kernel) {
        /* reproduce the CP0/TLB state RouterBOOT leaves at the hand-over */
        int t;

        env->CP0_Status |= (1 << CP0St_ERL) | (1 << 10);
        env->CP0_Config0 = 0x80000485;
        env->CP0_EntryHi = 0x800fc000;
        for (t = 0; t < 8; t++) {
            env->tlb->mmu.r4k.tlb[t].VPN = 0x800fc000 - t * 0x4000;
            env->tlb->mmu.r4k.tlb[t].PageMask = 0;
            env->tlb->mmu.r4k.tlb[t].PFN[0] = 0;
            env->tlb->mmu.r4k.tlb[t].PFN[1] = 0;
        }
    }
    if (s->elf_kernel) {
        uint32_t a0 = s->elf_argc, a1 = s->elf_argv, a2 = 0, a3 = 0;
        const char *ov;

        /*
         * Some bootloaders (MikroTik's, for the RouterOS kernel) use the
         * VxWorks-style convention where a0 == -2 marks a1 as the boot
         * argument block; the kernel only records that pointer in that case.
         */
        if ((ov = getenv("MT7621_ELF_A0")) != NULL) {
            a0 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A1")) != NULL) {
            a1 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A2")) != NULL) {
            a2 = strtoul(ov, NULL, 0);
        }
        if ((ov = getenv("MT7621_ELF_A3")) != NULL) {
            a3 = strtoul(ov, NULL, 0);
        }
        env->active_tc.gpr[4] = a0;
        env->active_tc.gpr[5] = a1;
        env->active_tc.gpr[6] = a2;
        env->active_tc.gpr[7] = a3;
    }

    /*
     * The MT7621 BootROM hands over with the 50 kHz systick already running
     * and its compare interrupt enabled; Breed only hooks GIC IRQ 5 and
     * relies on that.  Reproduce the hand-over state.
     */
    if (s->systick_timer) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        s->systick_regs[SYSTICK_CONFIG / 4] =
            SYSTICK_CFG_CNT_EN | SYSTICK_CFG_EXT_STK_EN;
        s->systick_regs[SYSTICK_COMPARE / 4] = 0xffff;
        s->systick_base_count = 0;
        s->systick_base_ns = now;
        mt7621_systick_set_pending(s, false);
        mt7621_systick_rearm(s);
    }

}

static MT7621Board mt7621_probe_board(const char *filename)
{
    uint8_t hdr[16];
    int fd;
    ssize_t n;
    uint32_t w;

    fd = open(filename, O_RDONLY | O_BINARY);
    if (fd < 0) {
        return MT7621_BOARD_LG_GAPM7200;
    }
    n = pread(fd, hdr, sizeof(hdr), 0);
    close(fd);
    if (n < 8) {
        return MT7621_BOARD_LG_GAPM7200;
    }
    /* uImage: the parallel-NAND layout used by Breed / OpenWrt. */
    if (ldl_be_p(hdr) == 0x27051956) {
        return MT7621_BOARD_LG_GAPM7200;
    }
    /*
     * RouterBOOT starts with "b 0x28" (beq zero,zero,+9) followed by a
     * zero delay slot and its stage header.  That header is only used by
     * SPI NOR boards, so treat it as the MikroTik preset.
     */
    w = ldl_le_p(hdr);
    if ((w >> 26) == 0x04 && (w & 0x03ffffff) == 9 &&
        ldl_le_p(hdr + 4) == 0) {
        return MT7621_BOARD_RB750GR3;
    }
    return MT7621_BOARD_LG_GAPM7200;
}

static void mt7621_apply_board(MT7621State *s, const char *filename)
{
    s->board = MT7621_BOARD_AUTO;
    if (s->board_name) {
        if (!strcmp(s->board_name, "lg-gapm7200") ||
            !strcmp(s->board_name, "gapm7200") ||
            !strcmp(s->board_name, "lg")) {
            s->board = MT7621_BOARD_LG_GAPM7200;
        } else if (!strcmp(s->board_name, "rb750gr3") ||
                   !strcmp(s->board_name, "mikrotik-rb750gr3") ||
                   !strcmp(s->board_name, "mikrotik")) {
            s->board = MT7621_BOARD_RB750GR3;
        } else {
            error_report("mt7621: unknown board '%s' (use lg-gapm7200 or "
                         "rb750gr3)", s->board_name);
            exit(1);
        }
    }
    if (s->board == MT7621_BOARD_AUTO) {
        s->board = mt7621_probe_board(filename);
    }

    if (s->board == MT7621_BOARD_RB750GR3) {
        s->chip_mode = 2;       /* SYSCFG0: boot from SPI 3-byte address */
    } else {
        s->chip_mode = MT7621_CHIP_MODE;    /* parallel NAND */
    }

    /*
     * Cache geometry lives in the 1004Kc model (cpu-defs.c.inc): 32 KiB,
     * 4-way, 32-byte lines.  Firmware and Linux both derive their flush
     * strategy from it, and Linux only installs r4k_blast_icache/dcache for
     * line sizes 16/32/64/128, so it must stay plausible.  MT7621_CFG1 can
     * still override the descriptor bits for experiments.
     */
}

static void mt7621_machine_init(MachineState *machine)
{
    MT7621State *s = MT7621_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    qemu_irq uart_irq;
    uint64_t ram_size = machine->ram_size;
    int i;

    mt7621_trace = getenv("MT7621_TRACE") != NULL;
    s->board = MT7621_BOARD_LG_GAPM7200;
    s->chip_mode = MT7621_CHIP_MODE;
    mt7621_apply_board(s, machine->kernel_filename);

    if (ram_size > 512 * MiB) {
        error_report("mt7621: at most 512 MiB of RAM is supported");
        exit(1);
    }

    /* CPU */
    s->cpuclk = clock_new(OBJECT(s), "cpu-refclk");
    clock_set_hz(s->cpuclk, 880 * 1000 * 1000);

    s->ncpus = machine->smp.cpus;
    if (s->ncpus < 1) {
        s->ncpus = 1;
    }
    if (s->ncpus > MT7621_MAX_CPUS) {
        s->ncpus = MT7621_MAX_CPUS;
    }
    for (i = 0; i < (int)s->ncpus; i++) {
        MIPSCPU *cpu = mips_cpu_create_with_clock(machine->cpu_type,
                                                  s->cpuclk, false);

        s->cpus[i] = cpu;
        cpu_mips_irq_init_cpu(cpu);
        cpu_mips_clock_init(cpu);
        qemu_register_reset(mt7621_cpu_reset, cpu);
        if (i) {
            CPU(cpu)->halted = 1;
        }
    }
    s->cpu = s->cpus[0];

    if (getenv("MT7621_CFG1")) {
        /*
         * Only the cache descriptor bits (Ds/Dl/Da at 15:7).  Masking the
         * whole low half would also clobber the feature bits, in particular
         * Config1.FP, and hide the FPU from the guest.
         */
        s->cfg1_cache_fields = strtoul(getenv("MT7621_CFG1"), NULL, 0) & 0xff80;
    }
    if (s->cfg1_cache_fields) {
        s->cpu->env.CP0_Config1 = (s->cpu->env.CP0_Config1 & ~0xff80u) |
                                  s->cfg1_cache_fields;
    }
    qemu_register_reset(mt7621_reset, s);

    /*
     * The GIC shared counter runs off the CPU clock on this SoC.  RouterBOOT
     * reports the real rate (875 MHz on the RB750Gr3) and OpenWrt programs its
     * clock events against it, so the model has to tick at the same rate or
     * every guest timeout is off by the ratio.
     */
    /*
     * The compressed-kernel loader sits at its load address and is then
     * overwritten by its own decompression output; on real MIPS it survives
     * only because its instructions are still in the I-cache (the kernel even
     * says "FIXME: should we flush cache here?").  Shadow the megabyte the
     * RB750Gr3 netboot images load into (0x80b81000 / 0x80b91000) so that
     * instruction fetch keeps seeing the loader, and leave every other address
     * - including the whole NAND/RouterOS boot - on plain TCG semantics.
     */
    if (!getenv("MT7621_NO_ICACHE") && s->board == MT7621_BOARD_RB750GR3) {
        uint64_t lo = 0x80b00000ULL, hi = 0x80c00000ULL;
        const char *rng = getenv("MT7621_ICACHE_RANGE");

        if (rng) {
            unsigned long long rlo = 0, rsize = 0;

            if (sscanf(rng, "%lli+%lli", &rlo, &rsize) == 2 && rsize) {
                lo = rlo;
                hi = rlo + rsize;
            }
        }
        icache_shadow_enable(lo, hi);
    }

    if (getenv("MT7621_GIC_FREQ")) {
        mips_gictimer_set_freq(strtoul(getenv("MT7621_GIC_FREQ"), NULL, 0));
    } else {
        /*
         * Run the GIC shared counter at the real device's CPU clock (875 MHz,
         * the rate RouterBOOT reports and OpenWrt programs its clockevents
         * against) for the whole machine, instead of the 25 MHz reference the
         * old hardcoded constant used.
         */
        mips_gictimer_set_freq(875 * 1000 * 1000);
    }


    s->poweroff_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     mt7621_poweroff_poll, s);
    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              (getenv("MT7621_ENTRYLOG") ? 1 : 250));

    if (getenv("MT7621_CMDLINE")) {
        s->cmdline_extra = g_strdup(getenv("MT7621_CMDLINE"));
        /*
         * RouterBOOT keeps the assembled command line in a scratch buffer
         * inside its RAM image.  Its address is build specific; the RB750Gr3
         * booter 3.35 uses this one.  Without a seed the poller falls back
         * to sweeping the whole bootloader window.
         */
        if (getenv("MT7621_CMDLINE_PHYS")) {
            s->cmdline_phys = strtoul(getenv("MT7621_CMDLINE_PHYS"), NULL, 0);
        } else if (s->board == MT7621_BOARD_RB750GR3) {
            s->cmdline_phys = 0x01200c80;
        }
        s->cmdline_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        mt7621_cmdline_poll, s);
        s->cmdline_deadline_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                 MT7621_CMDLINE_LIFETIME_NS;
        timer_mod(s->cmdline_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000000LL);
    }

    /* DDR */
    memory_region_init_ram(&s->ram, NULL, "mt7621.ddr", ram_size,
                           &error_fatal);
    memory_region_add_subregion(sysmem, MT7621_RAM_BASE, &s->ram);

    /*
     * The MT7621 DRAM controller only decodes the address bits that the
     * populated chips need, so accesses above the installed size wrap around
     * and alias onto low memory.  Breed detects the DDR size exactly this
     * way (write a pattern at 0, write 0 at 1<<n, read back 0), so the
     * aliases have to exist for it to measure less than 512 MiB.
     */
    if (ram_size & (ram_size - 1)) {
        error_report("mt7621: RAM size must be a power of two");
        exit(1);
    }
    s->ram_alias_count = MIN(MT7621_KSEG1_WINDOW / ram_size - 1,
                             ARRAY_SIZE(s->ram_alias));
    for (i = 0; i < s->ram_alias_count; i++) {
        memory_region_init_alias(&s->ram_alias[i], NULL, "mt7621.ddr-alias",
                                 &s->ram, 0, ram_size);
        memory_region_add_subregion(sysmem, (i + 1) * ram_size,
                                    &s->ram_alias[i]);
    }

    /*
     * Ethernet/sysctl blocks the firmware probes: reads must not return
     * 0xffffffff, which makes ring-size computations overflow.
     */
    memory_region_init_io(&s->eth, NULL, &mt7621_eth_ops, s, "mt7621.eth",
                          MT7621_ETH_SIZE);
    memory_region_add_subregion(sysmem, 0x1e100000, &s->eth);

    /*
     * The frame engine contains 24 KiB of SRAM at 0x1e108000.  The MT7621
     * BootROM and the pre-DDR boot stages use it as stack and scratch
     * space; RouterBOOT's DDR init even keeps its function table there.
     * It must be real memory, not part of the register file.
     */
    memory_region_init_ram(&s->fe_sram, NULL, "mt7621.fe-sram",
                           0x00008000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x1e108000, &s->fe_sram);

    /*
     * The SoC interrupt controller is a MIPS GIC at 0x1fbc0000 (not the
     * RT305x-style INTC of older Ralink parts).  Directing a shared
     * interrupt to CPU line 0 gives Cause IP2, which is the line both
     * Breed and Linux expect for it.
     *
     * Map it after the DDR aliases: those cover 1 GiB..2 GiB, which
     * contains 0x1fbc0000, and a later subregion wins.
     */
    s->gic = qdev_new(TYPE_MIPS_GIC);
    qdev_prop_set_uint32(s->gic, "num-vp", s->ncpus);
    qdev_prop_set_uint32(s->gic, "num-irq", MT7621_GIC_NUM_IRQ);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, MT7621_GIC_BASE);

    /*
     * Reset notifiers run in registration order, and the GIC registers its
     * own reset when the device is created, so register ours afterwards to
     * keep the SoC default mapping.
     */
    qemu_register_reset(mt7621_gic_reset, s);

    /*
     * Ethernet.  The frame engine raises GIC shared interrupt 3 and its
     * DMA rings move frames between guest memory and a QEMU net backend,
     * so a tap device on the host is bridged at layer 2.
     */
    {
        const char *p = getenv("MT7621_ETH_PORT");
        unsigned def_port = s->board == MT7621_BOARD_RB750GR3 ? 4 : 4;

        s->eth_port = p ? (unsigned)atoi(p) : def_port;
    }
    s->eth_irq = qdev_get_gpio_in(s->gic, 3);
    memset(s->eth_regs, 0, sizeof(s->eth_regs));

    mt7621_switch_defaults(s);
    mt7621_phy_defaults(s);

    /*
     * The frame engine's NIC is part of the SoC, so attach it to the user's
     * netdev (documented as id=net0) directly instead of going through the
     * pluggable-device matching.
     */
    {
        NetClientState *peer = qemu_find_netdev("net0");

        if (peer) {
            /* The NIC keeps a pointer to this, so it must outlive init. */
            memset(&s->nic_conf, 0, sizeof(s->nic_conf));
            qemu_macaddr_default_if_unset(&s->nic_conf.macaddr);
            s->nic_conf.peers.ncs[0] = peer;
            s->nic = qemu_new_nic(&mt7621_net_info, &s->nic_conf,
                                  "mt7621-eth", "mt7621-eth",
                                  &s->nic_guard, s);
            qemu_set_info_str(qemu_get_queue(s->nic), "mt7621-eth");
        }
    }

    /* palmbus */
    memory_region_init(&s->periph, NULL, "mt7621.periph",
                       MT7621_PERIPH_SIZE);

    mt7621_sysctl_defaults(s);
    memory_region_init_io(&s->sysctl, NULL, &mt7621_reg_ops, s->sysctl_regs,
                          "mt7621.sysctl", 0x1000);
    memory_region_init_io(&s->rstctl, NULL, &mt7621_rstctl_ops, s,
                          "mt7621.rstctl", 4);
    memory_region_add_subregion(&s->sysctl, SYS_RSTCTL, &s->rstctl);
    s->wdt_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mt7621_wdt_expire, s);
    memory_region_init_io(&s->wdt, NULL, &mt7621_wdt_ops, s,
                          "mt7621.wdt", MT7621_WDT_SIZE);
    memory_region_add_subregion(&s->sysctl, MT7621_WDT_OFF, &s->wdt);
    memory_region_init_io(&s->spi, NULL, &mt7621_spi_ops, s,
                          "mt7621.spi", 0x100);
    memory_region_add_subregion(&s->sysctl, MT7621_SPI_OFF, &s->spi);
    memory_region_add_subregion(&s->periph, MT7621_SYSCTL_OFF, &s->sysctl);

    /* systick: compare interrupt routed through the GIC */
    memset(s->systick_regs, 0, sizeof(s->systick_regs));
    s->systick_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->systick_base_count = 0;
    s->systick_irq = qdev_get_gpio_in(s->gic, MT7621_SYSTICK_IRQ);
    s->systick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    mt7621_systick_expire, s);
    s->systick_pulse_off = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                        mt7621_systick_pulse_release, s);
    memory_region_init_io(&s->systick, NULL, &mt7621_systick_ops, s,
                          "mt7621.systick", 0x10);
    memory_region_add_subregion(&s->periph, MT7621_SYSTICK_OFF,
                                &s->systick);

    mt7621_dramc_defaults(s);
    memory_region_init_io(&s->dramc, NULL, &mt7621_reg_ops, s->dramc_regs,
                          "mt7621.dramc", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_DRAMC_OFF, &s->dramc);

    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    memory_region_init_io(&s->nfi, NULL, &mt7621_nfi_ops, s,
                          "mt7621.nfi", 0x1000);
    memory_region_add_subregion(&s->periph, MT7621_NFI_OFF, &s->nfi);



    /* Coherence manager + cluster power controller */
    mt7621_cm_defaults(s);
    memory_region_init_io(&s->cm, NULL, &mt7621_cm_ops, s,
                          "mt7621.cm", MT7621_CM_SIZE);
    memory_region_add_subregion(sysmem, MT7621_CM_BASE, &s->cm);

    uart_irq = qdev_get_gpio_in(s->gic, MT7621_UART0_IRQ);
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
        uint8_t magic[4] = { 0 };
        int mfd = open(machine->kernel_filename, O_RDONLY | O_BINARY);

        s->image_file = g_strdup(machine->kernel_filename);
        if (mfd >= 0) {
            if (read(mfd, magic, 4) != 4) {
                magic[0] = 0;
            }
            close(mfd);
        }
        /*
         * A raw ELF here is a kernel (e.g. an OpenWrt initramfs image), not a
         * flash dump: load it like a bootloader would and run it.  Handy for
         * separating machine-model problems from bootloader ones.
         */
        if (ldl_be_p(magic) == 0x7f454c46) {
            mt7621_load_image(s, machine->kernel_filename);
        } else if (s->board == MT7621_BOARD_RB750GR3) {
            mt7621_nor_load(s, machine->kernel_filename);
        } else {
            mt7621_nand_load(s, machine->kernel_filename);
            mt7621_nand_attach(s);
            mt7621_load_image(s, machine->kernel_filename);
        }
    } else {
        error_report("mt7621: no boot image (-kernel) supplied");
        exit(1);
    }
}


static void mt7621_set_board(Object *obj, const char *value,
                             Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);

    g_free(s->board_name);
    s->board_name = g_strdup(value);
}

/*
 * Default RDUID value: a fixed, plausible Winbond unique ID.  Boards that
 * care about matching a real chip's identity override it with
 * -M mt7621,spi-uid=0x...
 */
#define MT7621_SPI_UID_DEFAULT 0xe4c1a75b3d9206f8ULL

static void mt7621_machine_instance_init(Object *obj)
{
    MT7621State *s = MT7621_MACHINE(obj);

    s->spi_uid = MT7621_SPI_UID_DEFAULT;
}

static void mt7621_set_spi_uid(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    MT7621State *s = MT7621_MACHINE(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    s->spi_uid = value;
}

static void mt7621_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    object_class_property_add_str(oc, "board", NULL, mt7621_set_board);
    object_class_property_set_description(oc, "board",
        "board preset: lg-gapm7200 (parallel NAND) or rb750gr3 (SPI NOR)");

    object_class_property_add(oc, "spi-uid", "uint64", NULL,
                              mt7621_set_spi_uid, NULL, NULL);
    object_class_property_set_description(oc, "spi-uid",
        "Winbond SPI NOR 64-bit unique ID reported by command 0x4b");

    mc->desc = "MediaTek MT7621 (MIPS 1004Kc)";
    mc->init = mt7621_machine_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("1004Kc");
    mc->default_ram_size = 256 * MiB;
    mc->default_ram_id = "mt7621.ram";
    mc->max_cpus = MT7621_MAX_CPUS;
    mc->default_cpus = 4;
}

static const TypeInfo mt7621_machine_type = {
    .name = TYPE_MT7621_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(MT7621State),
    .instance_init = mt7621_machine_instance_init,
    .class_init = mt7621_machine_class_init,
};

static void mt7621_machine_register_types(void)
{
    type_register_static(&mt7621_machine_type);
}

type_init(mt7621_machine_register_types);
