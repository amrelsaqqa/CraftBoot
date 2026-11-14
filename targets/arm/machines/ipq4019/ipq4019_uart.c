/*
 * Qualcomm IPQ4018/IPQ4019 -- BLSP UARTDM (the console).
 *
 * One UART is the console; the model is the register file the driver polls
 * plus the RX side of the BAM path (the guest's receive descriptors are
 * filled by the QUP DMA engine, not by this block).  Bytes written to the
 * transmitter are fed to the chardev and to state_note_tx(), which watches
 * for the booter's handoff line in the whole-machine state dump.
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/boards.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "system/memory.h"
#include "ipq4019_internal.h"

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

void uart_update_irq(IPQUart *u)
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
void bam_uart_rx_input_ready(void)
{
    uart_rx_fill(ipq->console_uart);
}

/* Run the idle timeout that retires a partially filled receive descriptor. */
void uart_rx_idle_arm(void)
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

bool uart_bam_rx_armed(void)
{
    return ipq->console_uart &&
           (ipq->console_uart->dmen & UART_DMEN_RX_BAM_ENABLE);
}

size_t uart_bam_rx_pending(void)
{
    return ipq->console_uart ? ipq->console_uart->rx_count : 0;
}

size_t uart_bam_rx_pull(uint8_t *buf, size_t n)
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
void uart_bam_rx_account(size_t n)
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

void add_uart(MemoryRegion *parent, hwaddr base, const char *name,
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
void uart_bam_write(const uint8_t *buf, size_t n)
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

