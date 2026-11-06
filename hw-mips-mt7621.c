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
#include "qemu/timer.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "target/mips/cpu-qom.h"
#include "target/mips/internal.h"

#define TYPE_MT7621_MACHINE MACHINE_TYPE_NAME("mt7621")
OBJECT_DECLARE_SIMPLE_TYPE(MT7621State, MT7621_MACHINE)

/* SoC memory map */
#define MT7621_RAM_BASE         0x00000000
#define MT7621_PERIPH_BASE      0x1e000000
#define MT7621_PERIPH_SIZE      0x00100000

#define MT7621_SYSCTL_OFF       0x00000000
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
#define MT7621_GIC_NUM_IRQ      256
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
#define SYS_CUR_CLK_STS         0x44
#define SYS_GPIO_MODE           0x60

#define SYSCTL_WORDS            (0x1000 / 4)
#define DRAMC_WORDS             (0x1000 / 4)
#define NFI_WORDS               (0x1000 / 4)

struct MT7621State {
    MachineState parent;

    MIPSCPU *cpu;
    Clock *cpuclk;
    DeviceState *gic;

    MemoryRegion ram;
    MemoryRegion periph;
    MemoryRegion sysctl;
    MemoryRegion rstctl;
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
    unsigned eth_port;          /* DSA user port exposed to the backend */
    uint32_t qtx_dtx;           /* forward DMA pointer (next to send) */
    uint32_t qtx_base;          /* start of the TX descriptor ring */
    bool qtx_en;                /* QDMA TX DMA enable */
    MemoryRegion cm;
    MemoryRegion eth;
    MemoryRegion ram_alias[8];
    int ram_alias_count;

    uint32_t sysctl_regs[SYSCTL_WORDS];
    uint32_t systick_regs[4];
    uint32_t dramc_regs[DRAMC_WORDS];
    uint32_t nfi_regs[NFI_WORDS];

    /* NAND flash contents and the current PIO transfer */
    uint8_t *nand;
    uint64_t nand_size;
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
    QEMUTimer *systick_timer;
    QEMUTimer *systick_pulse_off;
    qemu_irq systick_irq;
    int64_t systick_base_ns;
    uint32_t systick_base_count;
    bool systick_pending;

    /* boot image */
    uint32_t load_addr;
    uint32_t entry;
    bool uimage;
    char *image_file;           /* boot image, reloaded on reset */
};

static bool mt7621_trace;

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
    uint32_t val = regs[(addr & 0xfff) >> 2];

    if ((addr & 0xfff) == SYS_GPIO_DATA) {
        val = mt7621_gpio_inputs(regs);
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

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: reg write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }

    switch (addr & 0xfff) {
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

    regs[(addr & 0xfff) >> 2] = val;
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
    /* A connected PHY: 100BASE-TX full duplex, autonegotiation done. */
    static const uint16_t phy_up[32] = {
        [0]  = 0x1140,          /* BMCR: autoneg, full duplex */
        [1]  = 0x796d,          /* BMSR: link up, autoneg complete */
        [2]  = 0x0362,          /* PHY ID1 */
        [3]  = 0x5c30,          /* PHY ID2 */
        [4]  = 0x01e1,          /* advertise */
        [5]  = 0x45e1,          /* link partner ability */
        [9]  = 0x0300,          /* 1000BASE-T control */
    };
    /* Nothing plugged in: no link, no link partner, autoneg still running. */
    static const uint16_t phy_down[32] = {
        [0]  = 0x1140,
        [1]  = 0x7949,
        [2]  = 0x0362,
        [3]  = 0x5c30,
        [4]  = 0x01e1,
        [9]  = 0x0300,
    };

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
     */
    return (phy && phy == s->eth_port) ? phy_up[reg & 0x1f]
                                       : phy_down[reg & 0x1f];
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
        /* new RX ring: the DMA restarts at descriptor 0 */
        s->eth_regs[off >> 2] = val;
        s->rx_drx_idx = 0;
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
    uint32_t idx = s->rx_drx_idx;
    uint32_t desc, rxd1, rxd2, cap, crx;
    uint8_t tagged[1600];
    size_t flen;
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
     * Sanity guard against a desynchronised ring: never reuse the
     * descriptor the driver has just handed back.
     */
    crx = s->eth_regs[ETH_PRX_CRX_IDX0 / 4];
    if (idx == crx) {
        if (getenv("MT7621_ETHLOG")) {
            fprintf(stderr, "eth: rx drop ring-full idx=%u crx=%u\n", idx, crx);
        }
        return;
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
     * Insert the MTK DSA tag after the source MAC, exactly like the switch
     * does for a frame arriving on a user port.  mtk_tag_rcv() takes the
     * port number from the second byte and hands the frame to that slave.
     */
    if (len + 4 > sizeof(tagged)) {
        len = sizeof(tagged) - 4;
    }
    memcpy(tagged, buf, 12);
    tagged[12] = 0x00;                    /* untagged special tag */
    tagged[13] = s->eth_port & 0x3f;      /* source port */

    tagged[14] = 0x00;
    tagged[15] = 0x00;
    memcpy(tagged + 16, buf + 12, len - 12);
    flen = len + 4;

    if (flen > cap) {
        flen = cap;                        /* stay inside the driver buffer */
    }
    /*
     * PDMA_GLO_CFG has RX_2B_OFFSET set, so the engine starts the frame two
     * bytes into the descriptor buffer; the driver's skb_reserve() accounts
     * for that offset.
     */
    address_space_write(&address_space_memory, rxd1 + 2,
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
    s->eth_regs[ETH_PRX_DRX_IDX0 / 4] = idx;
    s->eth_regs[ETH_PDMA_INT_STATUS / 4] |= PDMA_RX_DONE;

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
                if (frame_fport == 1 && flen > 18) {
                    if ((frame[12] & 0xfe) <= 2 && !(frame[13] & 0xc0)) {
                        if (frame[13] & (1u << s->eth_port)) {
                            memmove(frame + 12, frame + 16, flen - 16);
                            flen -= 4;
                            forward = true;
                        }
                    } else {
                        forward = true;     /* untagged conduit frame */
                    }
                }

                if (forward && frame_tso) {
                    mt7621_eth_tx_tso(s, frame, flen);
                    forward = false;
                } else if (forward && frame_csum) {
                    mt7621_eth_tx_csum(frame, flen);
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

    off = (uint64_t)s->nfi_row * MT7621_NAND_PAGE_SIZE;
    if (s->nand && off < s->nand_size) {
        uint64_t n = s->nand_size - off;

        if (n > MT7621_NAND_PAGE_SIZE) {
            n = MT7621_NAND_PAGE_SIZE;
        }
        memcpy(buf, s->nand + off, n);
    }

    s->nfi_served = 0;
    mt7621_nfi_fifo_put(s, buf, sizeof(buf));

    /* Honour the column address: 2048..2111 selects the spare area. */
    if (s->nfi_col < sizeof(buf)) {
        s->nfi_fifo_pos = s->nfi_col;
    }
}

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
            uint64_t off = mt7621_nfi_addr_row(s) * MT7621_NAND_PAGE_SIZE;
            uint32_t n = s->nfi_wbuf_len;

            if (n > MT7621_NAND_PAGE_SIZE) {
                n = MT7621_NAND_PAGE_SIZE;
            }
            if (off + n <= s->nand_size) {
                memcpy(s->nand + off, s->nfi_wbuf, n);
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
                                 MT7621_NAND_PAGES_BLK * MT7621_NAND_PAGE_SIZE;
            uint64_t blk_size = (uint64_t)MT7621_NAND_PAGES_BLK * MT7621_NAND_PAGE_SIZE;

            if (blk_start + blk_size <= s->nand_size) {
                memset(s->nand + blk_start, 0xff, blk_size);
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
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
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
    uint32_t *regs = opaque;
    uint32_t val;

    /*
     * Breed releases every core through GCR_CL_OTHER and then polls
     * GCR_CO_COHERENCE until the core reports in.  We only model CPU0
     * (QEMU has no MT VPEs), so always report the domain as coherent.
     */
    if ((addr & (MT7621_CM_SIZE - 1)) == MT7621_GCR_OFF + 0x4008) {
        val = 1;
    } else {
        val = regs[(addr & (MT7621_CM_SIZE - 1)) >> 2];
    }

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: cm read  %08" HWADDR_PRIx
                      " -> %08x\n", addr, val);
    }
    return val;
}

static void mt7621_cm_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    uint32_t *regs = opaque;

    if (mt7621_trace) {
        qemu_log_mask(LOG_UNIMP, "mt7621: cm write %08" HWADDR_PRIx
                      " <- %08" PRIx64 "\n", addr, val);
    }
    regs[(addr & (MT7621_CM_SIZE - 1)) >> 2] = val;
}

static const MemoryRegionOps mt7621_cm_ops = {
    .read = mt7621_cm_read,
    .write = mt7621_cm_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

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
    s->sysctl_regs[SYS_SYSCFG0 / 4]      = 0x00000180 | MT7621_CHIP_MODE;
    s->sysctl_regs[SYS_CLKCFG0 / 4]      = 0x40000000; /* CPU_CLK_SEL = 1 */
    s->sysctl_regs[SYS_CUR_CLK_STS / 4]  = 0x00000101; /* /1 * 1 */
    /*
     * 0x3fc is polled for bit 10 before Breed's DDR3 init: it reports the
     * DDR/PLL calibration as finished. Our DDR is always ready.
     */
    s->sysctl_regs[0x3fc / 4]            = 0x00000400;
}

static void mt7621_cm_defaults(MT7621State *s)
{
    memset(s->cm_regs, 0, sizeof(s->cm_regs));
    s->cm_regs[(MT7621_GCR_OFF + GCR_CONFIG) / 4] = 0x00000004; /* 4 cores */
    s->cm_regs[(MT7621_GCR_OFF + GCR_BASE) / 4]   = 0x1fbf8000;
    s->cm_regs[(MT7621_GCR_OFF + GCR_REV) / 4]    = 0x00000002;
    /*
     * The MT7621 GIC is the legacy MIPS GIC at 0x1fbc0000, not a CM-managed
     * GIC; advertising one in the GCR makes Breed take a CM-GIC path that
     * does not exist on this SoC and crash.
     */
    s->cm_regs[(MT7621_GCR_OFF + GCR_GIC_STATUS) / 4] = 0x00000000;
    s->cm_regs[(MT7621_GCR_OFF + GCR_CPC_STATUS) / 4] = 0x00000001;
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
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 250);
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

    /* Everything the guest programmed is gone after a real reset. */
    memset(s->eth_regs, 0, sizeof(s->eth_regs));
    memset(s->nfi_regs, 0, sizeof(s->nfi_regs));
    s->qtx_dtx = 0;
    s->qtx_base = 0;
    s->qtx_en = false;
    s->rx_drx_idx = 0;
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
    if (s->image_file) {
        mt7621_load_image(s, s->image_file);
    }

    /* Malta does the same when a -kernel image is supplied. */
    env->CP0_Status &= ~(1 << 2); /* CP0St_ERL */
    env->active_tc.PC = s->entry;

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

static void mt7621_machine_init(MachineState *machine)
{
    MT7621State *s = MT7621_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    qemu_irq uart_irq;
    uint64_t ram_size = machine->ram_size;
    int i;

    mt7621_trace = getenv("MT7621_TRACE") != NULL;

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

    s->poweroff_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                     mt7621_poweroff_poll, s);
    timer_mod(s->poweroff_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 250);

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
     * The SoC interrupt controller is a MIPS GIC at 0x1fbc0000 (not the
     * RT305x-style INTC of older Ralink parts).  Directing a shared
     * interrupt to CPU line 0 gives Cause IP2, which is the line both
     * Breed and Linux expect for it.
     *
     * Map it after the DDR aliases: those cover 1 GiB..2 GiB, which
     * contains 0x1fbc0000, and a later subregion wins.
     */
    s->gic = qdev_new(TYPE_MIPS_GIC);
    qdev_prop_set_uint32(s->gic, "num-vp", 1);
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
        s->eth_port = p ? (unsigned)atoi(p) : 4;   /* lan1 */
    }
    s->eth_irq = qdev_get_gpio_in(s->gic, 3);
    memset(s->eth_regs, 0, sizeof(s->eth_regs));

    mt7621_switch_defaults(s);

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
    memory_region_init_io(&s->cm, NULL, &mt7621_cm_ops, s->cm_regs,
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
        s->image_file = g_strdup(machine->kernel_filename);
        mt7621_nand_load(s, machine->kernel_filename);
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
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("1004Kc");
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
