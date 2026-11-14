/*
 * Qualcomm IPQ4018/IPQ4019 -- TLMM pin controller and the board's visible
 * parts (lamps, keys).
 *
 * The pin controller is a wall of per-pin register blocks; only the SPI NOR
 * chip select is load-bearing for the boot chain, but gpiolib read-modify-
 * writes the whole block, so the model is a plain register file with a hook
 * per pin.  Lamps and keys are board wiring: they connect to pins, the panel
 * and the key device are qdevs, and the pins themselves come from the board
 * preset.
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "hw/boards.h"
#include "qapi/error.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/surface.h"
#include "qapi/util.h"
#include "qapi/qapi-types-ui.h"
#include "system/memory.h"
#include "ipq4019_internal.h"

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
void ipq_tlmm_connect(IPQTLMM *t, unsigned pin, IPQPinHandler fn,
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

void add_tlmm(MemoryRegion *parent, hwaddr base, hwaddr size,
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
void tlmm_dev_reset(IPQTLMM *t)
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

void ipq_board_add_leds(void)
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
void ipq_board_add_keys(void)
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
