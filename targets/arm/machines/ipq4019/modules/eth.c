/*
 * Qualcomm IPQ4018/IPQ4019 -- Ethernet: the IPQESS DMA engine, the host
 * netdev behind it, the QCA MDIO master and its clause-22 PHYs.
 *
 * The switch itself is not modelled: the EDMA moves frames between the
 * guest's rings and one QEMU net client, and the MDIO side answers the PHY
 * registers the driver polls (link, speed, MMD).  Board port lists come from
 * a device tree, like the pins.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "system/device_tree.h"
#include "hw/boards.h"
#include "net/net.h"
#include "net/checksum.h"
#include "net/eth.h"
#include <libfdt.h>
#include "ipq4019.h"

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
            if (rx != e->rx_level[q]) {
                e->rx_level[q] = rx;
                EDMA_DBG("rx irq q=%u %d (isr=%08x mask=%08x)\n", q, rx,
                         e->regs[EDMA_REG_RX_ISR / 4],
                         e->regs[EDMA_REG_RX_INT_MASK_Q(q) / 4]);
            }
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
    /*
     * Both index halves are ring-slot indices that wrap at the ring size
     * (the driver publishes (head + RING - 1) % RING and reads the engine's
     * consumer back the same way), so the room is the live modular
     * difference.  A changed RFD_BASE_Q starts a fresh ring, which is what
     * keeps a dirty RouterBOOT handover from poisoning this arithmetic.
     */
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
    EDMA_DBG("rx_ring prod=%u cons=%u ring=%u base=%08x free=%u len=%zu\n",
             prod, cons, ring, base, free, len);
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
        if (!i) {
            EDMA_DBG("rx slot=%u addr=%08x nbuf=%u len=%zu\n",
                     slot, addr, nbuf, len);
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
void edma_connect_netdev(IPQEdma *e)
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
    if (off == EDMA_REG_RFD_IDX_Q(0) &&
        env_u32("QEMU_IPQ_EDMA_LOG", 0) > 1) {
        fprintf(stderr, "edma: rfd_idx q=0 read -> %08x\n",
                e->regs[off / 4]);
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
            EDMA_DBG("rfd_idx q=%u write val=%08x old=%08x\n",
                     q, (uint32_t)val, e->regs[off / 4]);
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

    for (q = 0; q < IPQ_EDMA_QUEUES; q++) {
        if (off == EDMA_REG_RFD_BASE_Q(q) &&
            (uint32_t)val != e->regs[off / 4]) {
            /*
             * A new ring base means a new ring: the driver is (re)building
             * it, so the engine's consumer index restarts at slot 0.
             * RouterBOOT hands over with its own ring armed; without this
             * the first Linux publication would be measured against the
             * booter's producer index.  Only the consumer half may be
             * cleared: the OpenWrt ipqess driver publishes its buffers
             * (RFD_IDX writes) *before* setting the base, so zeroing the
             * producer half would forget buffers the driver will never
             * publish again, and the receive path would sit dead.
             */
            e->regs[EDMA_REG_RFD_IDX_Q(q) / 4] &= EDMA_RFD_PROD_MASK;
            EDMA_DBG("rfd_base q=%u new=%08x ring reset\n", q,
                     (uint32_t)val);
        }
    }

    e->regs[off / 4] = (uint32_t)val;
    if (off == EDMA_REG_RX_DESC0 || off == EDMA_REG_RFD_BASE_Q(0) ||
        off == EDMA_REG_RFD_IDX_Q(0)) {
        EDMA_DBG("reg write off=%03x val=%08x\n", (unsigned)off,
                 (uint32_t)val);
    }
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

IPQEdma *add_edma(MemoryRegion *parent, hwaddr base, hwaddr size)
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
void edma_wire_irqs(DeviceState *gicdev)
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

void phy_reset_regs(IPQPhy *p)
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

void mdio_add_from_dtb(IPQMdio *m, const char *path)
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
const char *ipq_machine_dtb(MachineState *machine)
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

IPQMdio *add_mdio(MemoryRegion *parent, const char *dtb_path)
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

