/*
 * Qualcomm IPQ4018/IPQ4019 -- the whole-machine state dump.
 *
 * A warm boot's failure is invisible in the guest's log: the booter prints the
 * same lines and hands the same image to the same stub.  What can differ is any
 * register the booter never rewrites, so this writes the model's whole register
 * state down at the handoff (QEMU_IPQ_STATE_DUMP=<path>) and a cold run can be
 * diffed against a warm one.  It also watches the console bytes for the
 * booter's handoff line.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "target/arm/cpu-qom.h"
#include "cpu.h"
#include "ipq4019.h"
#include "qpic.h"

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

void ipq_state_dump(const char *tag)
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
void state_note_tx(const uint8_t *buf, unsigned n)
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
