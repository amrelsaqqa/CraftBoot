/*
 * Qualcomm IPQ4018/IPQ4019 -- the QPIC parallel NAND controller.
 *
 * The register map (nand-qpic-common.h), the codeword layout the driver frames,
 * the die, and the BAM that moves the bytes.  Everything here is private to
 * modules/qpic.c except the two entry points at the bottom.
 */
#ifndef HW_ARM_IPQ4019_QPIC_H
#define HW_ARM_IPQ4019_QPIC_H

#include "ipq4019.h"

/* QPIC: the parallel NAND controller (and the BAM that feeds it)         */

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
#define QN_CFG0_CW_PER_PAGE     0x1c0u      /* bits 8:6, steps - 1 */

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

/*
 * The controller works a codeword at a time: one ECC step is 512 bytes of
 * page data, and a step takes QPIC_CW_SIZE bytes of the raw page once its
 * parity and spare bytes are counted in (2112 / 4 = 528 here).  The buffer at
 * FLASH_BUF_ACC holds exactly one such window: what a read location asks for
 * lands at its own offset inside the window, and a program consumes the same
 * window, so the two paths stay symmetric.
 */
#define QPIC_ECC_STEP   512u
#define QPIC_CW_STEPS   (NAND_PAGE / QPIC_ECC_STEP)
#define QPIC_CW_SIZE    (NAND_STRIDE / QPIC_CW_STEPS)
#define QPIC_IO_MAX     (NAND_STRIDE * 2u)  /* a page, plus one in flight */
#define QPIC_RX_DECS    32u                 /* queued rx descriptors */

struct IPQQpic {
    MemoryRegion mr;
    IPQParNand *die;

    uint32_t reg[IPQ_QPIC_NANDC_SIZE / 4];

    uint8_t buf[NAND_STRIDE];   /* one codeword window, at BUF_ACC offset 0 */
    bool loc_last;              /* the EXEC uses the LAST_CW read locations */

    uint8_t tx_stage[QPIC_IO_MAX];  /* program bytes the data pipe delivered */
    uint32_t tx_len;
    uint32_t txn_wr;            /* codeword the next program EXEC lands on */
    uint32_t tx_page;           /* page the staged bytes are consumed for */
    bool tx_page_set;
    bool tx_seq;                /* tx_page has seen a page-sequence window */
    uint32_t tx_used;           /* page-sequence bytes of tx_page programmed */
    bool tx_lost;               /* a staging overflow was reported */
    /*
     * A program EXEC whose payload was still in flight on the data pipe.
     * The driver rings the data descriptors and the command batch on two
     * different pipes and a doorbell only drains one of them, so the command
     * side can reach the EXEC before the bytes are staged.  Programming the
     * short page was silent corruption (the rest stayed queued and shifted
     * every later codeword), so the EXEC is held here and completed when the
     * data arrives.
     *
     * The entries belong to one page.  A window of a different page arriving
     * while one is still waiting means the staged bytes can no longer be
     * attributed to either page; the staging is dropped (loudly) rather than
     * shifted into the new page, because a shift corrupts every page after it.
     */
    struct {
        uint32_t page, at, cw, need;
        bool spare;
    } pgm_pend[8];
    unsigned pgm_n;

    uint8_t rd_stream[QPIC_IO_MAX]; /* bytes a read produced, not yet delivered */
    uint32_t rd_len;
    uint32_t rd_page;           /* page rd_stream is collecting */
    uint32_t txn_rd;            /* codeword the next read EXEC lands on */
    bool rd_lost;               /* a read overflow was reported */

    /*
     * A read puts its window into rd_stream and the BAM hands it to the rx
     * descriptors the data pipe queued; the two pipes can be rung in either
     * order (Linux queues the descriptors first, RouterBOOT runs the command
     * pipe first), so both ends just fill and drain the same FIFO.
     *
     * The page is read one codeword per EXEC, so the windows are appended to
     * the same stream and a reader that drains less than a whole window (the
     * booter takes the 64-byte EC header in 16-byte transfers) still gets the
     * page's bytes in order.  The stream is dropped when an EXEC addresses the
     * first codeword, or a different page: that is where a page read starts.
     */
    uint32_t rx_addr[QPIC_RX_DECS];
    uint32_t rx_len[QPIC_RX_DECS];
    uint32_t rx_done[QPIC_RX_DECS];
    uint32_t rx_n;

    uint32_t last_page;         /* page the window belongs to (logging) */
};

extern bool ipq_qpic_store_loaded;

/*
 * Protocol violations always print.  Each one means the model misread the
 * controller's stream, and every one of them costs correctness, so they are
 * reported without needing an environment variable; they are capped so a
 * systematic mismatch cannot flood the console.
 */
/* ---- the die ---------------------------------------------------------- */



void qpic_dev_reset(void);
void ipq_board_add_qpic_nand(MemoryRegion *sysmem);
void ipq_qpic_set_irq(qemu_irq irq);

#endif /* HW_ARM_IPQ4019_QPIC_H */
