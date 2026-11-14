/*
 * Qualcomm IPQ4018/IPQ4019 machine -- internal interface between blocks.
 *
 * The machine is one SoC: the same die, the same controllers, the same
 * addresses.  Everything outside it is a board question and lives as data in
 * ipq4019_board.c.  The C is split by controller, one file per address block,
 * because that is where the volume is and where a reader needs a boundary:
 *
 * present:
 *   ipq4019.c          machine init, memory map, reset, -M properties, dump
 *   modules/gpio.c     TLMM, LEDs, keys
 *   modules/uart.c     BLSP UARTDM
 *
 * planned (moved in this order):
 *   modules/flash.c    flash window, QUP SPI, NOR die, die store, SPI NAND
 *   modules/eth.c      IPQESS, host net backend, MDIO/PHY, board ports
 *   modules/bam.c      BLSP1 BAM (QUP side)
 *   modules/qpic.c     parallel NAND die and QPIC controller
 *   modules/qpic_bam.c QPIC BAM pipes and command elements
 *   modules/board.c    board presets, config merge, env shim
 *   modules/stub.c     shadowed stubs, register-file block, gcnt
 *   modules/dump.c     whole-machine state dump and diagnostics
 *
 * A block owns its register offsets, its MemoryRegionOps, its state and one
 * ipq4019_<block>_init(IPQ4019State *).  Anything another file needs is
 * declared here; nothing else crosses a file boundary, so this header is the
 * whole interface and it should stay small.
 *
 * Every source includes "qemu/osdep.h" first, then this header.
 */
#ifndef HW_ARM_IPQ4019_H
#define HW_ARM_IPQ4019_H

#include "system/memory.h"
#include "hw/qdev-core.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "hw/irq.h"

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

typedef struct IPQBoardConfig {
    bool nor;                   /* is there a NOR die on chip select 0? */
    uint32_t nor_size;          /* die bytes, power of two */
    const char *nor_id;         /* RDID as hex, e.g. "ef4019" */
    const char *nor_uid;        /* Unique-ID as hex, e.g. "0141ac3357ac0001" */
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
    /*
     * A board whose mainline device tree selects the NOR from a plain GPIO
     * instead of the QUP's own chip select.  The hAP ac3 is one: OpenWrt's
     * qcom-ipq4019-mikrotik-hap-ac3.dts drives `cs-gpios = <&tlmm 12
     * GPIO_ACTIVE_LOW>`, while the vendor tree (and RouterBOOT) use the QUP
     * select and pin 54.  Both are real wiring on that board, so the machine
     * listens on both and lets whichever guest is running pick one.  Zero
     * means the board has no such second select.
     */
    uint32_t nor_cs_gpio_alt;

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
    IPQ_CFG_NOR_UID         = 1u << 27,
};

typedef struct IPQBoardPreset {
    const char *name;
    IPQBoardConfig cfg;
} IPQBoardPreset;

/* ipq4019_gpio.c: the pin controller and the board's lamps/keys */
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

void add_tlmm(MemoryRegion *parent, hwaddr base, hwaddr size, const char *name);
void tlmm_dev_reset(IPQTLMM *t);
void ipq_tlmm_connect(IPQTLMM *t, unsigned pin, IPQPinHandler fn, void *opaque);
void ipq_board_add_leds(void);
void ipq_board_add_keys(void);

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


/* ipq4019_uart.c */
void add_uart(MemoryRegion *parent, hwaddr base, const char *name,
              Chardev *chr, qemu_irq irq);
void uart_update_irq(IPQUart *u);
bool uart_bam_rx_armed(void);
void uart_bam_write(const uint8_t *buf, size_t n);
void uart_bam_rx_account(size_t n);
void bam_uart_rx_input_ready(void);
void uart_rx_idle_arm(void);
size_t uart_bam_rx_pending(void);
size_t uart_bam_rx_pull(uint8_t *buf, size_t n);

/* ipq4019_dump.c: the console watcher lives with the dump */
void state_note_tx(const uint8_t *buf, unsigned n);

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

/* helpers shared by the blocks (defined where their state lives) */
const char *trace_pc(void);
void die_dbg_ser(uint8_t c);
void bam_uart_rx_idle(void);
void bam_uart_rx_kick(void);

/* file-scope siblings: one machine per process, so a plain pointer is enough */
extern IPQ4019State *ipq;
extern IPQBoardState *ipq_board;
extern IPQBoardConfig ipq_cfg;   /* the merged board: presets + properties + env */

/* small helpers shared by every block */
uint32_t env_u32(const char *name, uint32_t def);
bool env_true(const char *name);
void qpic_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void qpic_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* HW_ARM_IPQ4019_H */
