/*
 * Qualcomm IPQ4018/IPQ4019 -- BLSP1 BAM.
 *
 * The DMA engine behind the QUP FIFOs: descriptor rings, pipe doorbells and
 * the per-EE interrupt sources.  The QUP (SPI) and the UART both hand
 * transfers to it, so it is a block of its own rather than part of either.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "ipq4019.h"

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

void bam_wire_pipes(IPQBam *b)
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

void bam_update_irq(IPQBam *b);
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

/*
 * The console receive path, also defined with the UART model.  Linux's
 * msm_serial does not use the RX FIFO interrupt path on this SoC: it requests
 * a DMA channel, masks RXLEV/RXSTALE, sets RX_BAM_ENABLE in UARTDM_DMEN and
 * then only reads a batch of bytes out of its DMA buffer when the engine says
 * a transfer finished.  So input reaches userspace only if the BAM RX pipe is
 * modelled.
 */

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
void bam_uart_rx_idle(void)
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
void bam_uart_rx_kick(void)
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
void bam_update_irq(IPQBam *b)
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
void bam_seed_id(IPQBam *b)
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

IPQBam *add_bam(MemoryRegion *parent, hwaddr base, hwaddr size,
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

