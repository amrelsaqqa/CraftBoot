/*
 * Qualcomm IPQ4018/IPQ4019 -- the board: presets, merge order, env shim.
 *
 * The SoC model is fixed; everything outside it is a board question and a
 * board question belongs in configuration.  A preset is a compiled-in data
 * table that fills the same IPQBoardConfig the -machine properties fill, and
 * an explicit property always wins, whatever order it was given in.  The env
 * knobs keep working as a deprecated shim over the same struct.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "ipq4019.h"

/* ------------------------------------------------------------------ */
/* the board: what is soldered around the SoC                          */
/* ------------------------------------------------------------------ */

/*
 * The SoC model above is fixed: the same die, the same controllers, the same
 * addresses.  Everything outside it is a board question -- which dies hang off
 * the QUP, how big they are, what they identify as, which one the boot chain
 * comes from -- and a board question belongs in configuration, not in code.
 *
 *   -machine ipq4019,board=default          today's wiring (a preset)
 *   -machine ipq4019,board=none             no die at all: the pure SoC
 *   -machine ipq4019,nor=on,nand=on,boot=spi-nand,...
 *
 * A preset is a compiled-in data table, nothing more: it fills the same
 * IPQBoardConfig that the explicit properties fill, and an explicit property
 * always wins over the preset, whatever order they were given in.  That is
 * why the setters only record what was given (ipq_cfg_set): the merge happens
 * once, in ipq_board_config_resolve(), when the machine starts.
 *
 * The environment knobs (QEMU_IPQ_FLASH_*, QEMU_IPQ_NAND_*) keep working as a
 * deprecated shim over the same struct, so every command line written before
 * this existed still selects the same board.
 */

/* the mask-ROM flash types SBL1 is told about (boot_pbl_v1.c switches on it) */


/*
 * The reference design's switch: a QCA8075 exposes its five copper PHYs at
 * MDIO addresses 0..4 and its PSGMII control block at 5 (U-Boot's PSGMII_ID).
 * A board with a different switch, or fewer ports, names its own list.
 */








/*
 * The presets.  `default` is the board this machine has always built: a
 * 32 MiB W25Q256 NOR behind chip select 0, no NAND, booting from the NOR.
 * `none` is the bare SoC -- every part has to be named on the command line.
 *
 * `hap-ac2` is a real board, and every fact in it comes from that board's
 * own device tree (the 952G_5HacD2HnD tree inside the MikroTik dump, see
 * tools/dts2xml.py and tests/dts2xml-test.sh): a 32 MiB NOR and a 1 Gbit SPI
 * NAND on chip selects 0 and 1, five copper MDIO PHYs at addresses 0..4, and
 * three lamp lines -- user-led on pin 3, power-led on 0, the USB power line on
 * 2, active low.  The switch's PSGMII control block at address 5 is not in
 * that tree (RouterOS never addresses it) but it belongs to the switch, so
 * the machine puts it on the bus for every board that has one; see
 * IPQ_PSGMII_ADDR.
 *
 * What a preset deliberately cannot carry is a button's *behaviour*: the tree
 * says a switch is on pin 63 and another on pin 5, but not that the first one
 * should be Escape in QEMU.  That is a decision about the run, so it stays a
 * `keys=` property; the preset only knows the pins.
 */
static const IPQBoardPreset ipq_board_presets[] = {
    { "default", {
        .nor = true,
        .nor_size = IPQ_FLASH_DEFAULT,
        .nor_id = "ef4019",
        .nor_cs_gpio = 54,
        .nand = false,
        .nand_cs_gpio = 59,
        .boot = IPQ_BOOT_SPI_NOR,
    } },
    { "none", {
        .nor = false,
        .nand = false,
        .boot = IPQ_BOOT_NONE,
    } },
    { "hap-ac2", {
        .nor = true,
        .nor_size = 33554432,
        .nor_id = "ef4019",
        .nor_cs_gpio = 54,
        .nand = true,
        .nand_size = NAND_BYTES_DEF,
        .nand_id = "c212",
        .nand_cs_gpio = 59,
        .leds = "user-led:3;power-led:0;usb-power-off:2:al",
        .eth_ports = "0;1;2;3;4",
        .boot = IPQ_BOOT_SPI_NOR,
    } },
    /*
     * MikroTik hAP ac3 (RBD53iG-5HacD2HnD-TC), from its own device tree
     * (qcom-ipq4019-hap-ac3.dts): a 16 MiB NOR and a 128 MiB *parallel* NAND
     * behind the QPIC controller.  Every pin here is a `gpios =` line in that
     * tree; the NAND is a MX30LF1G18AC (its id is what the driver prints as
     * c2 f1 80 1d), 2048+64 byte pages in 64-page blocks.
     *
     * The board boots from the NOR in this preset because that is what the
     * dumps of it in circulation are; RouterBOOT finds the rest of the
     * installation on the NAND.
     */
    { "hap-ac3", {
        .nor = true,
        .nor_size = 16777216,
        .nor_id = "ef4018",
        .nor_cs_gpio = 54,
        /*
         * OpenWrt's device tree for this board selects the NOR from GPIO 12
         * (cs-gpios = <&tlmm 12 GPIO_ACTIVE_LOW>) and muxes the pin as plain
         * GPIO, because the QUP select on 54 is not wired to the die.  The
         * vendor tree and RouterBOOT use the QUP select instead, hence both.
         */
        .nor_cs_gpio_alt = 12,
        .qpic_nand = true,
        .qpic_nand_size = NAND_BYTES_DEF,
        .qpic_nand_id = "c2f1801d",
        .leds = "status-blue:0;status-red:1;status-green:3;wlan:23;"
                "ethernet:22;wan:28;lan1:29;lan2:30;lan3:31;lan4:32;poe:36",
        .eth_ports = "0;1;2;3;4",
        .boot = IPQ_BOOT_SPI_NOR,
    } },
};

/* what the machine was asked for, before presets and env are merged in */
IPQBoardConfig ipq_cfg_pending;
uint32_t ipq_cfg_set;
const char *ipq_cfg_board;
/* the resolved answer, built once when the machine starts */
IPQBoardConfig ipq_cfg;
static bool ipq_cfg_ready;

/* The preset names, in the order they are defined, for error messages. */
static const char *ipq_board_preset_names(void)
{
    static char names[128];
    size_t i;

    if (!names[0]) {
        for (i = 0; i < ARRAY_SIZE(ipq_board_presets); i++) {
            if (i) {
                g_strlcat(names, ", ", sizeof(names));
            }
            g_strlcat(names, ipq_board_presets[i].name, sizeof(names));
        }
    }
    return names;
}

static const IPQBoardPreset *ipq_board_preset(const char *name)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(ipq_board_presets); i++) {
        if (g_strcmp0(ipq_board_presets[i].name, name) == 0) {
            return &ipq_board_presets[i];
        }
    }
    return NULL;
}

/*
 * Hex bytes the way a datasheet writes an ID: "ef4019", "ef 40 19" and
 * "ef:40:19" all mean the same three bytes.  A plain strtoul() would swallow
 * "4019" as one number and leave the ID one byte long, so parse in pairs.
 */
int ipq_parse_hex_bytes(const char *s, uint8_t *out, int max,
                               const char *what)
{
    const char *p = s;
    int n = 0;

    while (*p && n < max) {
        int hi, lo;

        while (*p && !g_ascii_isxdigit(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        if (!p[1] || !g_ascii_isxdigit(p[1])) {
            n = -1;
            break;
        }
        hi = g_ascii_xdigit_value(p[0]);
        lo = g_ascii_xdigit_value(p[1]);
        out[n++] = (hi << 4) | lo;
        p += 2;
    }
    if (n <= 0) {
        error_report("ipq4019: %s='%s' is not a hex byte string like ef4019",
                     what, s);
        exit(1);
    }
    return n;
}

void ipq_board_config_resolve(MachineState *machine)
{
    const IPQBoardPreset *p;
    const char *env;
    uint32_t env_set = 0;
    IPQBoardConfig want = ipq_cfg_pending;

    if (ipq_cfg_ready) {
        return;
    }

    /* 1. the preset (board=<name>, default when nothing was said) */
    p = ipq_board_preset(ipq_cfg_board ? ipq_cfg_board : "default");
    if (!p) {
        error_report("ipq4019: no such board '%s'; known boards: %s",
                     ipq_cfg_board, ipq_board_preset_names());
        exit(1);
    }
    ipq_cfg = p->cfg;

    /* 2. explicit properties, which win over the preset */
    if (ipq_cfg_set & IPQ_CFG_NOR)        { ipq_cfg.nor        = want.nor; }
    if (ipq_cfg_set & IPQ_CFG_NOR_SIZE)   { ipq_cfg.nor_size   = want.nor_size; }
    if (ipq_cfg_set & IPQ_CFG_NOR_ID)     { ipq_cfg.nor_id     = want.nor_id; }
    if (ipq_cfg_set & IPQ_CFG_NOR_UID)    { ipq_cfg.nor_uid    = want.nor_uid; }
    if (ipq_cfg_set & IPQ_CFG_NOR_FILE)   { ipq_cfg.nor_file   = want.nor_file; }
    if (ipq_cfg_set & IPQ_CFG_NOR_DRIVE)  { ipq_cfg.nor_drive  = want.nor_drive; }
    if (ipq_cfg_set & IPQ_CFG_NOR_IMAGE)  { ipq_cfg.nor_image  = want.nor_image; }
    if (ipq_cfg_set & IPQ_CFG_NAND)       { ipq_cfg.nand       = want.nand; }
    if (ipq_cfg_set & IPQ_CFG_NAND_SIZE)  { ipq_cfg.nand_size  = want.nand_size; }
    if (ipq_cfg_set & IPQ_CFG_NAND_ID)    { ipq_cfg.nand_id    = want.nand_id; }
    if (ipq_cfg_set & IPQ_CFG_NAND_FILE)  { ipq_cfg.nand_file  = want.nand_file; }
    if (ipq_cfg_set & IPQ_CFG_NAND_DRIVE) { ipq_cfg.nand_drive = want.nand_drive; }
    if (ipq_cfg_set & IPQ_CFG_NAND_IMAGE) { ipq_cfg.nand_image = want.nand_image; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND)       { ipq_cfg.qpic_nand       = want.qpic_nand; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_SIZE)  { ipq_cfg.qpic_nand_size  = want.qpic_nand_size; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_ID)    { ipq_cfg.qpic_nand_id    = want.qpic_nand_id; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_FILE)  { ipq_cfg.qpic_nand_file  = want.qpic_nand_file; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_DRIVE) { ipq_cfg.qpic_nand_drive = want.qpic_nand_drive; }
    if (ipq_cfg_set & IPQ_CFG_QPIC_NAND_IMAGE) { ipq_cfg.qpic_nand_image = want.qpic_nand_image; }
    if (ipq_cfg_set & IPQ_CFG_BOOT)       { ipq_cfg.boot      = want.boot; }
    if (ipq_cfg_set & IPQ_CFG_NOR_CS)     { ipq_cfg.nor_cs_gpio  = want.nor_cs_gpio; }
    if (ipq_cfg_set & IPQ_CFG_NAND_CS)    { ipq_cfg.nand_cs_gpio = want.nand_cs_gpio; }
    if (ipq_cfg_set & IPQ_CFG_LEDS)       { ipq_cfg.leds        = want.leds; }
    if (ipq_cfg_set & IPQ_CFG_KEYS)       { ipq_cfg.keys        = want.keys; }
    if (ipq_cfg_set & IPQ_CFG_ETH_PORTS)  { ipq_cfg.eth_ports   = want.eth_ports; }
    if (ipq_cfg_set & IPQ_CFG_ETH_BRIDGE) { ipq_cfg.eth_bridge_port = want.eth_bridge_port; }
    if (ipq_cfg_set & IPQ_CFG_ETH_PHY_ID) { ipq_cfg.eth_phy_id  = want.eth_phy_id; }
    if (ipq_cfg_set & IPQ_CFG_POWEROFF)   { ipq_cfg.poweroff    = want.poweroff; }

    /*
     * 3. the deprecated environment shim.  It overwrites, because that is what
     *    it did before: a harness that exports QEMU_IPQ_NAND_PERSIST expects a
     *    NAND, whatever the machine type says.
     */
    if ((env = getenv("QEMU_IPQ_FLASH_PERSIST")) && *env) {
        ipq_cfg.nor_file = env;
        env_set |= IPQ_CFG_NOR_FILE | IPQ_CFG_NOR;
        if (ipq_cfg_set & IPQ_CFG_NOR && !want.nor) {
            fprintf(stderr, "ipq4019: nor=off is overridden by "
                    "QEMU_IPQ_FLASH_PERSIST\n");
        }
        ipq_cfg.nor = true;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_IMAGE")) && *env) {
        ipq_cfg.nor_image = env;
        env_set |= IPQ_CFG_NOR_IMAGE | IPQ_CFG_NOR;
        ipq_cfg.nor = true;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_SIZE")) && *env) {
        ipq_cfg.nor_size = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NOR_SIZE;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_JEDEC")) && *env) {
        ipq_cfg.nor_id = env;
        env_set |= IPQ_CFG_NOR_ID;
    }
    if ((env = getenv("QEMU_IPQ_FLASH_UID")) && *env) {
        ipq_cfg.nor_uid = env;
        env_set |= IPQ_CFG_NOR_UID;
    }
    if ((env = getenv("QEMU_IPQ_NAND_PERSIST")) && *env) {
        ipq_cfg.nand_file = env;
        ipq_cfg.nand = true;
        env_set |= IPQ_CFG_NAND_FILE | IPQ_CFG_NAND;
    } else if (env_true("QEMU_IPQ_NAND")) {
        ipq_cfg.nand = true;
        env_set |= IPQ_CFG_NAND;
    }
    if ((env = getenv("QEMU_IPQ_NAND_SIZE")) && *env) {
        ipq_cfg.nand_size = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NAND_SIZE;
    }
    if ((env = getenv("QEMU_IPQ_NAND_ID")) && *env) {
        ipq_cfg.nand_id = env;
        env_set |= IPQ_CFG_NAND_ID;
    }
    if ((env = getenv("QEMU_IPQ_CS_GPIO")) && *env) {
        ipq_cfg.nor_cs_gpio = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NOR_CS;
    }
    if ((env = getenv("QEMU_IPQ_NAND_CS_GPIO")) && *env) {
        ipq_cfg.nand_cs_gpio = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_NAND_CS;
    }
    if ((env = getenv("QEMU_IPQ_ETH_PORTS")) && *env) {
        ipq_cfg.eth_ports = env;
        env_set |= IPQ_CFG_ETH_PORTS;
    }
    if ((env = getenv("QEMU_IPQ_ETH_BRIDGE_PORT")) && *env) {
        ipq_cfg.eth_bridge_port = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_ETH_BRIDGE;
    }
    if ((env = getenv("QEMU_IPQ_ETH_PHY_ID")) && *env) {
        ipq_cfg.eth_phy_id = strtoul(env, NULL, 0);
        env_set |= IPQ_CFG_ETH_PHY_ID;
    }
    ipq_cfg_set |= env_set;

    /* 4. defaults and sanity, so the rest of the machine can trust the struct */
    if (ipq_cfg.nor) {
        if (!ipq_cfg.nor_size) {
            ipq_cfg.nor_size = IPQ_FLASH_DEFAULT;
        }
        if (!ipq_cfg.nor_id) {
            ipq_cfg.nor_id = "ef4019";
        }
        if (!ipq_cfg.nor_uid) {
            ipq_cfg.nor_uid = "0141ac3357ac0001";
        }
        if (!ipq_cfg.nor_cs_gpio) {
            ipq_cfg.nor_cs_gpio = 54;
        }
        if (ipq_cfg.nor_size < IPQ_FLASH_MIN ||
            ipq_cfg.nor_size > IPQ_FLASH_SIZE ||
            (ipq_cfg.nor_size & (ipq_cfg.nor_size - 1))) {
            error_report("ipq4019: nor-size=%u must be a power of two between "
                         "%u and %u bytes", ipq_cfg.nor_size,
                         (unsigned)IPQ_FLASH_MIN, (unsigned)IPQ_FLASH_SIZE);
            exit(1);
        }
    }
    if (ipq_cfg.nand) {
        if (!ipq_cfg.nand_size) {
            ipq_cfg.nand_size = NAND_BYTES_DEF;
        }
        if (!ipq_cfg.nand_id) {
            ipq_cfg.nand_id = "c212";       /* MX35LF1GE4AB */
        }
        if (!ipq_cfg.nand_cs_gpio) {
            ipq_cfg.nand_cs_gpio = 59;
        }
    }
    if (ipq_cfg.qpic_nand) {
        if (!ipq_cfg.qpic_nand_size) {
            ipq_cfg.qpic_nand_size = NAND_BYTES_DEF;
        }
        if (!ipq_cfg.qpic_nand_id) {
            ipq_cfg.qpic_nand_id = "c2f1801d";  /* MX30LF1G18AC */
        }
    }
    if (!ipq_cfg.eth_phy_id) {
        ipq_cfg.eth_phy_id = IPQ_ETH_DEFAULT_PHY_ID;
    }
    if (!(ipq_cfg_set & IPQ_CFG_POWEROFF)) {
        /* a machine whose guest cannot power it off is a machine that hangs */
        ipq_cfg.poweroff = true;
    }
    if (!ipq_cfg.eth_bridge_port) {
        /* port 0 is the CPU port: there is no cable to hand a host frame to */
        ipq_cfg.eth_bridge_port = 5;
    }
    if (ipq_cfg.eth_bridge_port > 6) {
        error_report("ipq4019: eth-bridge-port=%u must be 1..6",
                     ipq_cfg.eth_bridge_port);
        exit(1);
    }
    /* its geometry is checked where the part is built (ipq_board_add_nand) */

    /* the boot device has to exist: that is the whole point of naming it */
    if (ipq_cfg.boot != IPQ_BOOT_NONE) {
        bool have = ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? ipq_cfg.nor :
                    ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? ipq_cfg.nand :
                    ipq_cfg.qpic_nand;

        if (!have) {
            error_report("ipq4019: boot-device=%s but no %s die is configured "
                         "(add %s=on, or pick another boot device)",
                         ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "spi-nor" :
                         ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "spi-nand" :
                         "qpic-nand",
                         ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "NOR" :
                         ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "SPI NAND" :
                         "parallel NAND",
                         ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "nor" :
                         ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "nand" :
                         "qpic-nand");
            exit(1);
        }
    } else if (ipq_cfg.nor || ipq_cfg.nand || ipq_cfg.qpic_nand) {
        /* a board with dies but no boot strap: the NOR is what a strap does */
        ipq_cfg.boot = ipq_cfg.nor ? IPQ_BOOT_SPI_NOR :
                       ipq_cfg.nand ? IPQ_BOOT_SPI_NAND : IPQ_BOOT_QPIC_NAND;
    }

    /*
     * -kernel is content, not wiring: it fills the boot device's die.  A
     * machine with no boot device at all cannot use it, and says so instead of
     * quietly inventing a NOR the way it used to.
     */
    if (machine->kernel_filename && ipq_cfg.boot == IPQ_BOOT_NONE) {
        error_report("ipq4019: -kernel %s given, but this board has no boot "
                     "device to put it in (board=none); add nor=on or "
                     "nand=on, or use -machine board=default",
                     machine->kernel_filename);
        exit(1);
    }
    if (machine->kernel_filename && !ipq_cfg.nor_image && !ipq_cfg.nand_image
        && !ipq_cfg.qpic_nand_image) {
        if (ipq_cfg.boot == IPQ_BOOT_SPI_NOR) {
            ipq_cfg.nor_image = machine->kernel_filename;
        } else if (ipq_cfg.boot == IPQ_BOOT_SPI_NAND) {
            ipq_cfg.nand_image = machine->kernel_filename;
        } else if (ipq_cfg.boot == IPQ_BOOT_QPIC_NAND) {
            ipq_cfg.qpic_nand_image = machine->kernel_filename;
        }
    }

    fprintf(stderr, "ipq4019: board %s: nor ", ipq_cfg_board ? ipq_cfg_board
            : "default");
    if (ipq_cfg.nor) {
        fprintf(stderr, "%u MiB cs gpio %u", (unsigned)(ipq_cfg.nor_size / MiB),
                ipq_cfg.nor_cs_gpio);
        if (ipq_cfg.nor_cs_gpio_alt) {
            fprintf(stderr, "/%u", ipq_cfg.nor_cs_gpio_alt);
        }
        fprintf(stderr, "%s, ",
                (ipq_cfg.nor_file || ipq_cfg.nor_drive) ? " (store)" : "");
    } else {
        fprintf(stderr, "none, ");
    }
    fprintf(stderr, "nand ");
    if (ipq_cfg.nand) {
        fprintf(stderr, "%u MiB cs gpio %u%s, ",
                (unsigned)(ipq_cfg.nand_size / MiB), ipq_cfg.nand_cs_gpio,
                (ipq_cfg.nand_file || ipq_cfg.nand_drive) ? " (store)" : "");
    } else {
        fprintf(stderr, "none, ");
    }
    fprintf(stderr, "qpic-nand ");
    if (ipq_cfg.qpic_nand) {
        if (ipq_cfg.qpic_nand_size % MiB) {
            fprintf(stderr, "%u bytes", ipq_cfg.qpic_nand_size);
        } else {
            fprintf(stderr, "%u MiB", (unsigned)(ipq_cfg.qpic_nand_size / MiB));
        }
        fprintf(stderr, "%s, ",
                (ipq_cfg.qpic_nand_file || ipq_cfg.qpic_nand_drive) ?
                " (store)" : "");
    } else {
        fprintf(stderr, "none, ");
    }
    fprintf(stderr, "boot-device %s\n",
            ipq_cfg.boot == IPQ_BOOT_SPI_NOR ? "spi-nor" :
            ipq_cfg.boot == IPQ_BOOT_SPI_NAND ? "spi-nand" :
            ipq_cfg.boot == IPQ_BOOT_QPIC_NAND ? "qpic-nand" : "none");

    ipq_cfg_ready = true;
}

