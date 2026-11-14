/*
 * Qualcomm IPQ4018/IPQ4019 -- the QPIC parallel NAND controller.
 *
 * The die model (column-addressed program, block erase, store persist) and the
 * controller state machine (codeword windows, BAM pipes, command elements).
 * The register layout and the state types are in qpic.h.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "qpic.h"

bool ipq_qpic_store_loaded;

void qpic_warn(const char *fmt, ...)
{
    static unsigned n;
    va_list ap;

    if (++n > 40) {
        if (n == 41) {
            fprintf(stderr, "qpic: anomaly: further reports suppressed\n");
        }
        return;
    }
    fprintf(stderr, "qpic: anomaly: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}


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
        uint32_t at = col + i;

        /*
         * The bad-block marker is the one part of the spare area a codeword
         * program never touches: the QPIC's BAD BLOCK BYTE (0x1d1, the last
         * codeword's byte 465, which is spare byte 1) is excluded from ECC
         * programming, and for a 2048+64 part the marker lives at spare
         * bytes 0..1.  What a codeword program carries for that area is the
         * driver's ECC placeholder -- the part gets computed parity there
         * instead -- and storing those placeholders verbatim zeroed the
         * marker, so every programmed block read back as bad: yaffs could
         * not remount its own writes ("block %d is bad"), and RouterBOOT
         * could not read a boot partition that was written as yaffs.
         * Spare-only programs (the ones that do move the marker when a block
         * is retired) still program it.
         */
        if (col < NAND_PAGE && at >= NAND_PAGE && at <= NAND_PAGE + 1) {
            continue;
        }
        d->data[off + at] &= buf[i];
    }
    qpic_die_persist(d, d->data + off, off, NAND_STRIDE);
}

/* ---- the controller --------------------------------------------------- */

static void qpic_log_stream(IPQQpic *q, const char *what, uint32_t addr,
                            uint32_t off, const uint8_t *p, uint32_t len)
{
    if (!env_true("QEMU_IPQ_QPIC_LOG")) {
        return;
    }
    qpic_log("qpic: %s %08x +%u from page %u cw off %u: "
             "%02x %02x %02x %02x %02x %02x %02x %02x\n",
             what, addr, len, q->last_page, off,
             p[0], len > 1 ? p[1] : 0, len > 2 ? p[2] : 0,
             len > 3 ? p[3] : 0, len > 4 ? p[4] : 0,
             len > 5 ? p[5] : 0, len > 6 ? p[6] : 0,
             len > 7 ? p[7] : 0);
}

/* How many codewords the CFG0 register says the page is split into. */
static uint32_t qpic_cw_steps(IPQQpic *q)
{
    return ((q->reg[QN_DEV0_CFG0 / 4] & QN_CFG0_CW_PER_PAGE) >> 6) + 1;
}

/*
 * The stride between two codewords is the CFG0 "user data" size, not the
 * page's raw stride: with ECC on, MikroTik's driver frames a codeword as 516
 * bytes (500 data + 16 spare on the last step) and its chunks tile the flat
 * 2048-byte page exactly; with ECC off the frame is the whole 528.  Sizing
 * the frame from the page stride instead put every chunk 16 bytes further
 * than the driver sent it, which scrambled the page: yaffs' write verify
 * ("write required 321 attempts", "disk is too small") and RouterBOOT's read
 * of an installer-written kernel both failed on it.
 */
static uint32_t qpic_cw_size(IPQQpic *q)
{
    uint32_t ud = (q->reg[QN_DEV0_CFG0 / 4] >> 9) & 0x3ff;

    return ud ? ud : QPIC_CW_SIZE;
}

/*
 * One read EXEC fills the window for the codeword it addresses.
 *
 * The read-location registers are the whole description of that window: the
 * driver sets them before each EXEC and the controller presents, at
 * FLASH_BUF_ACC + offset, the bytes that live at that offset inside the
 * codeword.  Walking the locations in order also gives the byte stream the
 * data pipe will drain (data first, then the spare bytes), which is what the
 * queued rx descriptors expect.
 *
 * A codeword's place in the page is its column when the command carries one
 * (raw reads and the last-codeword helpers address it that way) and otherwise
 * the running step index, because the ECC path leaves the column at 0 and
 * lets the controller advance by itself.
 *
 * The column is an absolute byte offset into the raw page, not a step number:
 * the bad-block helpers read the last codeword with column 528 * (steps - 1)
 * while CFG0 still describes a single step, so deriving the codeword from the
 * step counter put the window at offset 0 and made every programmed block look
 * bad (its data byte landed where the BBM byte is looked for).
 */
static void qpic_read_window(IPQQpic *q, uint32_t page, uint32_t op,
                             uint32_t col, uint32_t cw)
{
    uint32_t base = col ? col : cw * qpic_cw_size(q);
    uint32_t regs = q->loc_last ? QN_RD_LOC_LAST0 : QN_RD_LOC0;
    const uint8_t *die = page < q->die->pages ?
                         q->die->data + (size_t)page * NAND_STRIDE : NULL;
    int i;

    /*
     * A read transaction is one page (the four codeword EXECs of a page
     * read) or one column-addressed window (the bad-block helpers address the
     * spare area that way and the pipe only ever sees those four bytes).
     * Anything still in the stream when a new transaction starts was never
     * collected by the data pipe, so a descriptor the guest queued will never
     * be filled; say so instead of silently prepending stale bytes.
     */
    if (cw == 0 || col != 0 || page != q->rd_page) {
        if (q->rd_len) {
            qpic_warn("read page %u starts with %u byte(s) uncollected\n",
                      page, q->rd_len);
        }
        q->rd_len = 0;
        q->rd_page = page;
        q->rd_lost = false;
    }

    for (i = 0; i < 4; i++) {
        uint32_t v = q->reg[regs / 4 + i];
        uint32_t off = v & 0x3ff;
        uint32_t len = (v >> 16) & 0x3ff;
        uint32_t k;

        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            qpic_log("qpic:   loc%d v=%08x off=%u len=%u base=%u rd=%u\n",
                     i, v, off, len, base, q->rd_len);
        }
        for (k = 0; k < len && off + k < NAND_STRIDE; k++) {
            uint32_t d = base + off + k;
            uint8_t b = (die && d < NAND_STRIDE) ? die[d] : 0xff;

            q->buf[off + k] = b;
            if (q->rd_len < QPIC_IO_MAX) {
                q->rd_stream[q->rd_len++] = b;
            } else if (!q->rd_lost) {
                q->rd_lost = true;
                qpic_warn("read page %u: window overflow at %u byte(s)\n",
                          page, q->rd_len);
            }
        }
        if (v & (1u << 31)) {
            break;              /* this location ends the window */
        }
    }
    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        uint32_t n = q->rd_len;
        qpic_log("qpic: win page=%u base=%u regs=%08x %08x %08x %08x die=%d "
                 "got=%02x %02x %02x %02x die8=%02x %02x %02x %02x %02x %02x %02x %02x\n",
                 page, base, q->reg[regs / 4], q->reg[regs / 4 + 1],
                 q->reg[regs / 4 + 2], q->reg[regs / 4 + 3],
                 die != NULL,
                 n > 3 ? q->rd_stream[n - 4] : 0xff,
                 n > 2 ? q->rd_stream[n - 3] : 0xff,
                 n > 1 ? q->rd_stream[n - 2] : 0xff,
                 n > 0 ? q->rd_stream[n - 1] : 0xff,
                 die && base + 0 < NAND_STRIDE ? die[base + 0] : 0xff,
                 die && base + 1 < NAND_STRIDE ? die[base + 1] : 0xff,
                 die && base + 2 < NAND_STRIDE ? die[base + 2] : 0xff,
                 die && base + 3 < NAND_STRIDE ? die[base + 3] : 0xff,
                 die && base + 4 < NAND_STRIDE ? die[base + 4] : 0xff,
                 die && base + 5 < NAND_STRIDE ? die[base + 5] : 0xff,
                 die && base + 6 < NAND_STRIDE ? die[base + 6] : 0xff,
                 die && base + 7 < NAND_STRIDE ? die[base + 7] : 0xff);
        if (col == 1584 && die) {
            qpic_log("qpic: bbm page=%u byte=%02x bblk=%u\n",
                     page, die[NAND_PAGE], page / NAND_PPBLK);
        }
    }
}

/*
 * How many bytes one program EXEC takes out of the data pipe.
 *
 * Linux queues the whole page before the first EXEC (516, 516, 516, then the
 * last codeword's 500 bytes), while MikroTik's older driver fed one codeword
 * per EXEC.  The windows tile the 2048-byte page exactly, so the size comes
 * from that tiling and from the CFG0 step size the driver framed the buffer
 * with -- not from the physical codeword, which also counts the ECC parity
 * and spare bytes the driver never sends.
 *
 * The last step is where this matters: for an ECC page it carries the rest of
 * the page (2048 - 3 * 516 = 500).  Sizing it as data + spare + parity + cfg
 * bits (527 for the traced configuration) made the held EXEC wait for bytes
 * that never arrive, and the next page's bytes then completed it: the stream
 * shifted by 27 bytes per page, the last-window write ran past the page into
 * the spare area, and every later page was written at the wrong offset.
 */
static uint32_t qpic_program_size(IPQQpic *q, uint32_t cw)
{
    uint32_t ecc = q->reg[QN_DEV0_ECC_CFG / 4];
    uint32_t steps = qpic_cw_steps(q);
    uint32_t n = qpic_cw_size(q);

    if ((ecc & 1) || steps <= 1) {
        return n;               /* ECC off: the step is one raw window */
    }
    if (cw + 1 == steps && (steps - 1) * n < NAND_PAGE) {
        return NAND_PAGE - (steps - 1) * n;
    }
    return n;
}

/* Bytes one complete program transaction moves for the configured layout. */
static uint32_t qpic_program_page_bytes(IPQQpic *q)
{
    uint32_t steps = qpic_cw_steps(q);

    if ((q->reg[QN_DEV0_ECC_CFG / 4] & 1) || steps <= 1) {
        return qpic_cw_size(q);
    }
    return MIN((uint32_t)NAND_PAGE, steps * qpic_cw_size(q));
}

static void qpic_program_do(IPQQpic *q, uint32_t page, uint32_t at,
                            uint32_t cw, uint32_t n)
{
    if (n) {
        if (env_true("QEMU_IPQ_QPIC_LOG") ||
            (env_true("QEMU_IPQ_QPIC_PGM") && cw == 0)) {
            qpic_log("qpic: prog page=%u cw=%u at=%u n=%u tx=%02x %02x %02x %02x "
                     "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     page, cw, at, n,
                     q->tx_stage[0], q->tx_stage[1], q->tx_stage[2],
                     q->tx_stage[3], q->tx_stage[4], q->tx_stage[5],
                     q->tx_stage[6], q->tx_stage[7], q->tx_stage[8],
                     q->tx_stage[9], q->tx_stage[10], q->tx_stage[11],
                     q->tx_stage[12]);
        }
        qpic_die_program(q->die, page, at, q->tx_stage, n);
        q->tx_len -= n;
        memmove(q->tx_stage, q->tx_stage + n, q->tx_len);
        ipq_die_store_sync(&q->die->store);
    }
}

/* Complete held EXECs, in the order they were issued, as data arrives. */
static void qpic_program_pending(IPQQpic *q)
{
    while (q->pgm_n && q->tx_len >= q->pgm_pend[0].need) {
        uint32_t page = q->pgm_pend[0].page;
        uint32_t at = q->pgm_pend[0].at;
        uint32_t cw = q->pgm_pend[0].cw;
        uint32_t need = q->pgm_pend[0].need;
        bool spare = q->pgm_pend[0].spare;

        qpic_program_do(q, page, at, cw, need);
        if (!spare) {
            q->tx_used += need;
        }
        memmove(&q->pgm_pend[0], &q->pgm_pend[1],
                (q->pgm_n - 1) * sizeof(q->pgm_pend[0]));
        q->pgm_n--;
    }
}

static void qpic_exec(IPQQpic *q)
{
    uint32_t cmd = q->reg[QN_CMD / 4];
    uint32_t op = cmd & QN_CMD_OP_MASK;
    uint32_t page = ((q->reg[QN_ADDR1 / 4] & 0xff) << 16) |
                    ((q->reg[QN_ADDR0 / 4] >> 16) & 0xffff);
    uint32_t col = q->reg[QN_ADDR0 / 4] & 0xffff;
    uint32_t steps = qpic_cw_steps(q);
    /*
     * The erase command is the odd one out: it carries the row in ADDR0[15:0]
     * (ADDR1[7:0] for the high bits) instead of ADDR0[31:16].  Reading it like
     * a page command made every erase land on page 0, so the blocks being
     * formatted were never erased and the filesystem could not be mounted.
     */
    uint32_t row_erase = col | ((q->reg[QN_ADDR1 / 4] & 0xff) << 16);

    if (env_true("QEMU_IPQ_QPIC_LOG")) {
        qpic_log(
                      "qpic: exec op=%u page=%u col=%u cmd=%08x addr0=%08x "
                      "addr1=%08x cfg0=%08x ecc=%08x loc=%08x in=%u\n",
                      op, page, col, cmd,
                      q->reg[QN_ADDR0 / 4], q->reg[QN_ADDR1 / 4],
                      q->reg[QN_DEV0_CFG0 / 4], q->reg[QN_DEV0_ECC_CFG / 4],
                      q->reg[QN_RD_LOC0 / 4], q->tx_len);
    }

    switch (op) {
    case QN_OP_RESET_DEVICE:
        q->txn_rd = q->txn_wr = 0;
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
        qpic_die_erase(q->die, row_erase);
        q->txn_rd = q->txn_wr = 0;
        break;

    case QN_OP_PAGE_READ:
    case QN_OP_PAGE_READ_ECC:
    case QN_OP_PAGE_READ_SPARE:
    case QN_OP_ONFI_READ: {
        uint32_t cw;

        if (col) {
            cw = MIN(col / QPIC_CW_SIZE, steps - 1);
        } else {
            if (q->txn_rd >= steps) {
                q->txn_rd = 0;
            }
            cw = q->txn_rd;
        }
        q->last_page = page;
        qpic_read_window(q, page, op, col, cw);
        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            qpic_log("qpic: rd window page=%u cw=%u col=%u len=%u loc=%08x\n",
                     page, cw, col, q->rd_len, q->reg[QN_RD_LOC0 / 4]);
        }
        q->txn_rd = cw + 1;
        break;
    }

    case QN_OP_PROGRAM_PAGE:
    case QN_OP_PROGRAM_PAGE_ECC:
    case QN_OP_PROGRAM_SPARE: {
        uint32_t cw = col ? MIN(col / QPIC_CW_SIZE, steps - 1) :
                            MIN(q->txn_wr, steps - 1);
        /* Same rule as the read side: a column is an absolute byte offset. */
        uint32_t at = col ? col : cw * qpic_cw_size(q);
        uint32_t need = qpic_program_size(q, cw);
        /*
         * A column-addressed program is a window of its own (spare bytes, the
         * bad-block marker): it is not a step of the four-codeword page
         * sequence, so it carries exactly what the pipe delivered for it,
         * bounded by the rest of the row.
         */
        bool spare = col != 0;
        uint32_t n;

        if (spare && at < NAND_STRIDE) {
            need = MIN(need, NAND_STRIDE - at);
        }

        /*
         * The staged bytes belong to one page.  A window of a different page
         * arriving while one is still waiting for its payload means the two
         * can no longer be told apart (the driver has moved on), so the
         * staging is dropped instead of shifted into the new page.
         */
        if (q->pgm_n && q->tx_page_set && page != q->tx_page) {
            qpic_warn("program page %u supersedes page %u with %u window(s) "
                      "still waiting (%u staged, %u used); dropping them\n",
                      page, q->tx_page, q->pgm_n, q->tx_len, q->tx_used);
            q->pgm_n = 0;
            q->tx_len = 0;
        }
        if (!q->tx_page_set || page != q->tx_page) {
            /*
             * A page whose windows did not add up to one transaction is the
             * other way the stream loses its alignment; the next page would
             * then be written shifted.
             */
            if (q->tx_page_set && q->tx_seq &&
                q->tx_used != qpic_program_page_bytes(q)) {
                qpic_warn("program page %u moved %u byte(s), expected %u\n",
                          q->tx_page, q->tx_used,
                          qpic_program_page_bytes(q));
            }
            q->tx_page = page;
            q->tx_page_set = true;
            q->tx_used = 0;
            q->tx_seq = false;
        }
        q->tx_lost = false;
        if (!spare) {
            q->tx_seq = true;
        }

        /*
         * Programming a NAND only clears bits, and the page keeps whatever
         * was never written, so the die ANDs the step in place.  The bytes
         * land where the read path will look for them: at the codeword's own
         * window, which is also the byte order the driver queued them in.
         *
         * A codeword is programmed only once its whole payload is staged:
         * consuming a short page would leave the remainder queued and shift
         * every later codeword into the wrong place.
         */
        n = spare ? MIN(q->tx_len, need) : need;
        if (n && q->tx_len >= n) {
            qpic_program_do(q, page, at, cw, n);
            if (!spare) {
                q->tx_used += n;
            }
        } else if (q->pgm_n < ARRAY_SIZE(q->pgm_pend)) {
            q->pgm_pend[q->pgm_n].page = page;
            q->pgm_pend[q->pgm_n].at = at;
            q->pgm_pend[q->pgm_n].cw = cw;
            q->pgm_pend[q->pgm_n].need = n ? n : need;
            q->pgm_pend[q->pgm_n].spare = spare;
            q->pgm_n++;
            if (env_true("QEMU_IPQ_QPIC_LOG")) {
                qpic_log("qpic: prog wait page=%u cw=%u need=%u staged=%u\n",
                         page, cw, need, q->tx_len);
            }
        } else {
            qpic_warn("program page %u cw=%u need=%u: wait queue full, "
                      "window dropped\n", page, cw, need);
        }
        q->last_page = page;
        q->txn_wr = cw + 1;
        break;
    }

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
         *
         * Bit 23 is the WP# pin, and it is active low: MikroTik's erase path
         * caches this word and, when the bit is clear, refuses the erase with
         * "msm_nand_erase: Device is write protected" before touching the
         * part.  The die here is writable, so the pin is deasserted and the
         * bit reads back set.  (Bits 4, 8 and 16 are the DEV/OP/MPU error
         * flags the same code checks, and they stay clear.)
         */
        return 0x20u | (1u << 23);
    }
    if (off == QN_BUFFER_STATUS) {
        return 0;                               /* no ECC errors */
    }
    if (off == QN_VERSION) {
        /*
         * NAND_VERSION, mainline's field layout: major in bits 31:28, minor in
         * 27:16 (nand-qpic-common.h).  A v2 controller is 0x20000000; the old
         * 0x00020000 put "2" in the minor field, so MikroTik's
         * msm_nand_version_check read major 0.0, took its fallback path, and
         * never got an id out of the part (flashid=eeeeeeee).
         */
        return 0x20000000u;                     /* QPIC v2.0 */
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

    /*
     * A fresh address starts a fresh transaction: the ECC path writes the
     * page address once and then steps through the codewords with the column
     * left at zero, so the step counter is what says which codeword an EXEC
     * means.  Raw reads and the last-codeword helpers put the codeword in the
     * column instead, which qpic_exec prefers when it is set.
     */
    if (off == QN_ADDR0) {
        q->txn_rd = 0;
        q->txn_wr = 0;
    }
    /*
     * The read locations come in two banks and the one written last is the
     * one this EXEC uses: the first three steps use READ_LOCATION_0..3 and the
     * last step uses READ_LOCATION_LAST_CW_0..3 (qpic v2), whichever step it
     * is.  Tracking the bank keeps the model from having to guess from the
     * column, which the ECC path never sets.
     */
    if (off >= QN_RD_LOC0 && off < QN_RD_LOC0 + 16) {
        q->loc_last = false;
    }
    if (off >= QN_RD_LOC_LAST0 && off < QN_RD_LOC_LAST0 + 16) {
        q->loc_last = true;
    }

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
    uint32_t irq_observed;      /* sources the CPU last saw in BAM_IRQ_SRCS */
    unsigned irq_line;          /* this model's current output level */
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
    uint32_t pending = qpic_bam_irq_pending(b);
    uint32_t fresh;

    if (!b->irq || !env_u32("QEMU_IPQ_QPIC_IRQ", 1)) {
        return;
    }
    if (!pending) {
        /*
         * Every source acknowledged: re-arm the edge for the next transfer.
         */
        b->irq_observed = 0;
        if (b->irq_line) {
            b->irq_line = 0;
            qemu_set_irq(b->irq, 0);
        }
        return;
    }
    /*
     * The line asserts when a pipe reports an enabled source the CPU has not
     * observed in BAM_IRQ_SRCS_EE yet, and drops when the CPU reads that
     * register -- an edge, not a held level.  A held level cannot cross the
     * RouterBOOT handover: its final read-ahead command batches complete
     * after its last wait_pipe(), so the command pipe keeps PRCSD/TRNSFR_END
     * latched with nobody left to acknowledge it (the booter waits on the
     * data pipe and jumps), and the kernel's SPS handler exits without
     * clearing sources of pipes it does not own.  A held level would re-fire
     * the SPI forever from that leftover -- the soft lockup the moment the
     * kernel enables the IRQ -- while an edge dies after one IRQ.  Only the
     * edge semantics are consistent with the same booter and kernel binaries
     * running on the silicon, where RouterOS boots from NAND.
     */
    fresh = pending & ~b->irq_observed;
    if (fresh && !b->irq_line) {
        b->irq_line = 1;
        qemu_set_irq(b->irq, 1);
    }
}

static unsigned qpgm_logs;

static void qpic_bam_data_in(IPQQpic *q, uint32_t addr, uint32_t len)
{
    uint8_t chunk[512];
    uint32_t done = 0;

    while (done < len) {
        uint32_t take = MIN(len - done, (uint32_t)sizeof(chunk));

        address_space_read(&address_space_memory, addr + done,
                           MEMTXATTRS_UNSPECIFIED, chunk, take);
        if (env_true("QEMU_IPQ_QPIC_PGM") && qpgm_logs < 400) {
            qpgm_logs++;
            qpic_log("qpic: datain addr=%08x len=%u stage=%u tx=%02x %02x "
                     "%02x %02x %02x %02x %02x %02x\n",
                     addr + done, take, q->tx_len,
                     chunk[0], chunk[1], chunk[2], chunk[3],
                     chunk[4], chunk[5], chunk[6], chunk[7]);
        }
        qpic_log_stream(q, "in", addr + done, q->tx_len, chunk, take);
        /*
         * The bytes stay staged until the command pipe reaches the program
         * EXEC they belong to, because the whole page arrives before the
         * first codeword is programmed.
         */
        if (q->tx_len + take <= QPIC_IO_MAX) {
            memcpy(q->tx_stage + q->tx_len, chunk, take);
            q->tx_len += take;
        } else if (!q->tx_lost) {
            q->tx_lost = true;
            qpic_warn("program staging overflow: %u byte(s) dropped\n", take);
        }
        done += take;
    }
    qpic_program_pending(q);
}

/*
 * Drain the descriptors the data pipe queued for a read.
 *
 * They are served from rd_stream, which the command pipe filled one codeword
 * at a time, so the guest sees the die's contents and not whatever the
 * controller's window happened to hold when the data pipe was rung.
 *
 * A descriptor whose length runs past the bytes produced so far simply waits:
 * the transfer is completed by the next read (or by this same pass, when the
 * command pipe ran after the data pipe).
 */
static void qpic_bam_data_out(IPQQpic *q)
{
    uint32_t pos = 0;

    while (q->rx_n && pos < q->rd_len) {
        uint32_t addr = q->rx_addr[0];
        uint32_t len = q->rx_len[0];
        uint32_t done = q->rx_done[0];
        uint8_t chunk[512];
        uint32_t take = MIN(MIN(len - done, (uint32_t)sizeof(chunk)),
                            q->rd_len - pos);
        uint32_t k;

        for (k = 0; k < take; k++) {
            chunk[k] = q->rd_stream[pos + k];
        }
        qpic_log_stream(q, "out", addr + done, pos, chunk, take);
        address_space_write(&address_space_memory, addr + done,
                            MEMTXATTRS_UNSPECIFIED, chunk, take);
        pos += take;
        q->rx_done[0] = done + take;
        if (q->rx_done[0] == len) {
            memmove(&q->rx_addr[0], &q->rx_addr[1],
                    (q->rx_n - 1) * sizeof(q->rx_addr[0]));
            memmove(&q->rx_len[0], &q->rx_len[1],
                    (q->rx_n - 1) * sizeof(q->rx_len[0]));
            memmove(&q->rx_done[0], &q->rx_done[1],
                    (q->rx_n - 1) * sizeof(q->rx_done[0]));
            q->rx_n--;
        }
    }
    if (pos) {
        memmove(q->rd_stream, q->rd_stream + pos, q->rd_len - pos);
        q->rd_len -= pos;
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
        if (c == 1) {
            /*
             * Register -> memory.  The destination is `data` plus the *upper
             * four bits* of the mask field (qcom_bam_dma.h, BAM v1.6+); taking
             * the mask as a full 32-bit high word turns a destination of
             * 0x8eaf408c into 0xffffffff8eaf408c when the driver fills the
             * mask like a write command, and the store then goes nowhere --
             * which is how an id read came back as the driver's 0xEE poison.
             */
            /*
             * The destination is the 32-bit `data` field.  The mask field does
             * not carry address bits here: MikroTik's driver fills it with
             * 0xffffffff on read commands (a copy of its write mask) while the
             * address it wants is a plain RAM pointer, so honouring the top
             * nibble would send every register read into no-man's land.
             */
            uint64_t dst = (uint64_t)data;
            uint32_t v = qpic_reg_read(q, off);

            qpic_log("qpic: ce read off=%#x dst=%08x%08x mask=%08x -> %08x\n",
                     off + QN_CE_BASE, (uint32_t)(dst >> 32), data, hi, v);
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
        if (env_true("QEMU_IPQ_QPIC_LOG")) {
            uint32_t i = p->cons, k;

            qpic_log("qpic: ring pipe=%d cons=%u tail=%u\n",
                     n, p->cons, tail);
            for (k = 0; k < 8 && i != tail; k++, i = (i + 1) % slots) {
                uint32_t e[2];

                address_space_read(&address_space_memory,
                                   p->desc_fifo + i * 8,
                                   MEMTXATTRS_UNSPECIFIED, e, sizeof(e));
                qpic_log("qpic:   slot %u: %08x %08x\n", i, e[0], e[1]);
            }
        }
        /*
         * The window produced by the command pipe stays in rd_stream until a
         * queued rx descriptor collects it: the pipes can be rung in either
         * order, and a transfer may run across several EXECs.
         */
        /*
         * The two pipes can be rung in either order.  Linux queues the rx
         * descriptors first and lets the command pipe run the read; RouterBOOT
         * runs the command pipe first and only then hands over the rx
         * descriptors.  Both are served here: whichever half arrives second
         * completes the transfer.
         */
        while (p->cons != tail && guard++ < 4096) {
            uint32_t d[2];

            address_space_read(&address_space_memory,
                               p->desc_fifo + p->cons * 8,
                               MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
            if (env_true("QEMU_IPQ_QPIC_LOG")) {
                qpic_log("qpic: desc pipe=%d cons=%u tail=%u db=%#x "
                         "addr=%08x w1=%08x\n",
                         n, p->cons, tail, doorbell, d[0], d[1]);
            }
            if (d[1] & 0xffff) {
                if (n == QB_PIPE_CMD) {
                    qpic_bam_ce(b, d[0], d[1] & 0xffff);
                } else if (p->ctrl & QB_P_DIRECTION) {
                    if (env_true("QEMU_IPQ_QPIC_LOG")) {
                        qpic_log(
                                      "qpic: rx desc %08x len %u\n",
                                      d[0], d[1] & 0xffff);
                    }
                    if (q->rx_n < ARRAY_SIZE(q->rx_addr)) {
                        q->rx_addr[q->rx_n] = d[0];
                        q->rx_len[q->rx_n] = d[1] & 0xffff;
                        q->rx_done[q->rx_n] = 0;
                        q->rx_n++;
                    } else {
                        qpic_warn("rx descriptor ring full: %08x len %u "
                                  "dropped\n", d[0], d[1] & 0xffff);
                    }
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
        /*
         * A command pass may have produced a window while descriptors were
         * already queued, and a data pass may have queued descriptors for a
         * window that was produced earlier.  Both end here.
         */
        qpic_bam_data_out(q);
    }
    /*
     * A doorbell only completes something if this pass retired a descriptor.
     * The driver's last ring of a transfer can be a bare flush -- RouterBOOT
     * times out on a part with no kernel and rings once more, and the
     * installer's flash tool ends its command list the same way.  Latching
     * PRCSD for those left a source nobody acknowledges, and the next kernel
     * waited for BAM_IRQ_SRCS_EE to reach zero forever (soft lockup inside
     * msm_nand_bam_init); skipping the latch made RouterBOOT's own wait loop
     * miss completions and hang the other way.
     */
    if (guard) {
        p->irq_stts |= QB_P_IRQ_PRCSD | QB_P_IRQ_TRNSFR_END;
        p->irq_stts &= ~QB_P_IRQ_OUT_DESC;
        qpic_bam_update_irq(b);
    }
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
    if (off == QB_IRQ_SRCS_EE && env_true("QEMU_IPQ_BAM_DBG")) {
        static unsigned calls;
        calls++;
        if (calls < 12 || (calls % 500000) == 0) {
            int i;
            fprintf(stderr, "bam: srcs=%08x", qpic_bam_irq_srcs(b));
            for (i = 0; i < QB_PIPES; i++) {
                fprintf(stderr, " p%d{ctrl=%08x sts=%08x en=%08x cons=%u evnt=%08x}",
                        i, b->pipe[i].ctrl, b->pipe[i].irq_stts,
                        b->pipe[i].irq_en, b->pipe[i].cons,
                        b->pipe[i].evnt_reg);
            }
            fprintf(stderr, "\n");
        }
    }
    if (off == QB_IRQ_SRCS_EE || off == QB_IRQ_SRCS) {
        /*
         * The CPU observing the sources is what retires the line's edge (see
         * qpic_bam_update_irq): the sources stay latched, but the SPI drops
         * so a leftover no driver owns cannot re-fire it.
         */
        b->irq_observed = qpic_bam_irq_pending(b);
        if (b->irq_line) {
            b->irq_line = 0;
            if (env_u32("QEMU_IPQ_QPIC_IRQ", 1)) {
                qemu_set_irq(b->irq, 0);
            }
        }
    }
    if (env_true("QEMU_IPQ_QPIC_LOG") && off == QB_IRQ_SRCS_EE) {
        qpic_log("qpic-bam: read %#x -> %08x%s\n", (uint32_t)off,
                 qpic_bam_irq_srcs(b),
                 env_u32("QEMU_IPQ_QPIC_LOG", 0) > 1 ? trace_pc() : "");
    }
    switch (off) {
    case QB_CTRL:        return b->ctrl;
    case QB_DESC_TRSHLD: return b->desc_trshld;
    case QB_CNFG_BITS:   return b->cfg_bits;
    case QB_IRQ_STTS:    return b->irq_stts;
    case QB_IRQ_EN:      return b->irq_en;
    case QB_REVISION:
        /*
         * {major:8 at 23:16, minor:8 at 15:12, num_ees:4 at 11:8, step:8}.
         * The EE count is not decoration: Linux's bam_init() range-checks the
         * node's qcom,ee against it and fails the probe with -EINVAL when it
         * reads zero, so the QPIC's BAM has to report the same four EEs the
         * BLSP1 one does (bam_seed_id()).
         */
        return 0x00010407u;                     /* v1.0, 4 EEs, step 7 */
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
        qpic_log("qpic-bam: write %#x <- %08x%s\n",
                      (uint32_t)off, (uint32_t)val,
                      env_u32("QEMU_IPQ_QPIC_LOG", 0) > 1 ? trace_pc() : "");
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
void qpic_dev_reset(void)
{
    IPQQpic *q = ipq_board ? ipq_board->qpic : NULL;
    IPQQpicBam *b = ipq_qpic_bam;
    int i;

    if (!q) {
        return;
    }
    memset(q->reg, 0, sizeof(q->reg));
    memset(q->buf, 0xff, sizeof(q->buf));
    q->tx_len = 0;
    q->txn_wr = 0;
    q->tx_page = UINT32_MAX;
    q->tx_page_set = false;
    q->tx_seq = false;
    q->tx_used = 0;
    q->tx_lost = false;
    q->pgm_n = 0;
    q->rd_len = 0;
    q->rd_lost = false;
    q->rd_page = UINT32_MAX;
    q->txn_rd = 0;
    q->rx_n = 0;
    q->loc_last = false;
    q->last_page = UINT32_MAX;

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
    b->irq_observed = 0;
    b->irq_line = 0;
    for (i = 0; i < QB_PIPES; i++) {
        memset(&b->pipe[i], 0, sizeof(b->pipe[i]));
    }
    qpic_bam_update_irq(b);
}

void ipq_board_add_qpic_nand(MemoryRegion *sysmem)
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
/* ------------------------------------------------------------------ */

/* The machine owns the GIC: it hands the BAM its SPI line after realisation. */
void ipq_qpic_set_irq(qemu_irq irq)
{
    if (ipq_qpic_bam) {
        ipq_qpic_bam->irq = irq;
    }
}
