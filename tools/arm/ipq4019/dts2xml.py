#!/usr/bin/env python3
"""
dts2xml -- turn a board device tree into this machine's configuration.

The device tree is the board: it is the document that says a QCA8075 answers at
MDIO addresses 0..4, that a lamp sits on TLMM pin 3 and is active low, that a
button is on pin 63, that the boot flash is an m25p80 on the QUP's chip select
0.  QEMU cannot read that tree itself -- a QEMU machine is configured with
-machine properties and libvirt adds its own arguments on top -- so this
translates one into the other, in the two forms a QEMU user needs:

    ./tools/dts2xml.py board.dtb > board.xml        # a libvirt domain
    ./tools/dts2xml.py board.dtb --print-argv      # a plain command line
    ./tools/dts2xml.py board.dtb --print-machine   # just the property list

What the tree can say, and what it cannot, is kept strictly apart:

  * says:    which PHYs are on the MDIO bus (their addresses, and whether the
             node is disabled), which GPIO lines have lamps (and their
             polarity), which have buttons, and which flash chips exist.
  * cannot:  how big a die is (an m25p80 node names its chip select, not its
             bytes), what a button should do in QEMU (a qcode), which switch
             port the host's cable is on, and which die RouterBOOT will boot
             from.  Those come from options, because they are decisions about
             *this* QEMU run rather than facts about the board -- and inventing
             them here would put fiction in a file that is supposed to be
             derived from the hardware.

The dies are handed to the machine as block backends (`-drive if=none,id=...`
plus `nor-drive=`/`nand-drive=`), which is the same pair of arguments a libvirt
<disk> turns into.  The die files are not written by this tool.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
import os
import shlex
import struct
import sys
from xml.sax.saxutils import escape, quoteattr

FDT_MAGIC = 0xD00DFEED

# The tokens of the flattened device tree's structure block.
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9

# The reference design's switch: the machine's own built-in population, used
# only when a tree has no mdio node at all.
GRANDFATHERED_PORTS = "0,1,2,3,4,5"

# Properties whose first cell is the GPIO controller, then #gpio-cells cells.
GPIO_PROPS = ("gpios", "gpio", "reset-gpios", "enable-gpios")


class Node:
    __slots__ = ("name", "props", "children", "parent")

    def __init__(self, name, parent=None):
        self.name = name
        self.props = {}
        self.children = []
        self.parent = parent

    # ---- a few typed readers, because the FDT stores bytes and nothing else

    def string(self, prop, default=None):
        v = self.props.get(prop)
        if v is None:
            return default
        return v.split(b"\0", 1)[0].decode("utf-8", "replace")

    def strings(self, prop):
        v = self.props.get(prop)
        if v is None:
            return []
        return [s.decode("utf-8", "replace") for s in v.split(b"\0") if s]

    def cells(self, prop):
        v = self.props.get(prop)
        if v is None or len(v) % 4:
            return []
        return list(struct.unpack(">%dI" % (len(v) // 4), v))

    def cell(self, prop, default=None):
        c = self.cells(prop)
        return c[0] if c else default

    def name_plain(self):
        """The node name without its unit address (`mdio@90000` -> `mdio`)."""
        return self.name.split("@", 1)[0]

    def compatible(self):
        return self.strings("compatible")

    def has_compatible(self, wanted):
        for c in self.compatible():
            for w in wanted:
                if c == w or c.startswith(w + ","):
                    return True
        return False

    def path(self):
        parts = []
        n = self
        while n is not None and n.parent is not None:
            parts.append(n.name)
            n = n.parent
        return "/" + "/".join(reversed(parts))

    def walk(self):
        for c in self.children:
            yield c
            yield from c.walk()


def parse_fdt(data):
    """Read the flattened tree into Nodes: a header, a struct block, a string
    block, and offsets -- nothing version-specific."""
    if len(data) < 40:
        raise ValueError("file is too short to be a device tree")
    (magic, _total, off_struct, off_strings, _off_rsv,
     _version, _last, _cpu, size_strings, _size_struct) = struct.unpack(
        ">10I", data[:40])
    if magic != FDT_MAGIC:
        raise ValueError("not a device tree (magic 0x%08x)" % magic)

    strings = data[off_strings:off_strings + size_strings]
    pos = off_struct
    root = None
    stack = []

    def align4(p):
        return (p + 3) & ~3

    while pos < len(data):
        (tok,) = struct.unpack(">I", data[pos:pos + 4])
        pos += 4
        if tok == FDT_NOP:
            continue
        if tok == FDT_END:
            break
        if tok == FDT_BEGIN_NODE:
            end = data.index(b"\0", pos)
            name = data[pos:end].decode("utf-8", "replace")
            pos = align4(end + 1)
            node = Node(name, stack[-1] if stack else None)
            if stack:
                stack[-1].children.append(node)
            else:
                root = node
            stack.append(node)
        elif tok == FDT_END_NODE:
            if not stack:
                raise ValueError("unbalanced node terminator")
            stack.pop()
        elif tok == FDT_PROP:
            length, nameoff = struct.unpack(">II", data[pos:pos + 8])
            pos += 8
            end = strings.index(b"\0", nameoff)
            pname = strings[nameoff:end].decode("ascii", "replace")
            value = data[pos:pos + length]
            pos = align4(pos + length)
            if not stack:
                raise ValueError("property outside any node")
            stack[-1].props[pname] = value
        else:
            raise ValueError("unknown FDT token %d at 0x%x" % (tok, pos - 4))
    if root is None:
        raise ValueError("device tree has no root node")
    return root


def load(path):
    data = open(path, "rb").read()
    if data[:4] == b"\xd0\x0d\xfe\xed":
        return parse_fdt(data)
    # A source tree: let dtc compile it, the same way a build does.
    import subprocess
    dtb = subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", "-", path],
                         check=True, capture_output=True)
    return parse_fdt(dtb.stdout)


# --------------------------------------------------------------------------
# what a board tree says
# --------------------------------------------------------------------------

def gpio_owner(root, phandle):
    """The controller a gpios entry points at, so #gpio-cells can be read."""
    for n in [root] + list(root.walk()):
        if n.cell("phandle") == phandle or n.cell("linux,phandle") == phandle:
            return n
    return None


def first_gpio(root, node):
    """(pin, active_low) for a node's first gpios entry, or None."""
    for prop in GPIO_PROPS:
        cells = node.cells(prop)
        if not cells:
            continue
        phandle = cells[0]
        owner = gpio_owner(root, phandle)
        width = owner.cell("#gpio-cells", 2) if owner else 2
        if len(cells) < 1 + width:
            return None
        pin = cells[1]
        flags = cells[2] if width >= 2 else 0
        # bit 0 of the flags is GPIO_ACTIVE_LOW; the rest (open drain, pull)
        # describes the electrics, which this machine does not model.
        return pin, bool(flags & 1)
    return None


def led_nodes(root):
    """The node holding lamps: gpio-leds, and MikroTik's own leds-rb."""
    for n in [root] + list(root.walk()):
        if n.has_compatible(("gpio-leds", "leds-rb", "gpio-keys",
                             "gpio-keys-polled")):
            return n
    return None


def clean_name(name, taken):
    """A name the machine's `name:pin[:al]` grammar can carry."""
    out = "".join(c if c.isalnum() or c in "._-" else "_" for c in name)
    out = out.strip("_") or "led"
    base, i = out, 2
    while out in taken:
        out = "%s%d" % (base, i)
        i += 1
    taken.add(out)
    return out


def board_from_tree(root):
    """The facts: PHYs on the bus, lamps and buttons, flash chips."""
    facts = {"phys": None, "leds": [], "keys": [], "nor": False, "nand": False,
             "model": root.string("model", "")}

    # --- the MDIO bus ---------------------------------------------------
    mdio = None
    for n in [root] + list(root.walk()):
        if n.has_compatible(("qcom,ipq40xx-mdio", "qcom,ipq4019-mdio")):
            mdio = n
            break
        if n.name_plain() == "mdio" and n.path().startswith("/soc/"):
            mdio = n
            break
    if mdio is not None:
        phys = []
        for phy in mdio.children:
            if not phy.name.startswith("ethernet-phy"):
                continue
            addr = phy.cell("reg")
            if addr is None:
                try:
                    addr = int(phy.name.split("@", 1)[1], 16)
                except (IndexError, ValueError):
                    continue
            speed = phy.cell("max-speed")
            entry = str(addr)
            if speed:
                entry += ":%d" % speed
            if phy.string("status") == "disabled":
                # A PHY the board leaves unpopulated: no cable, no link.
                entry += "!"
            phys.append(entry)
        if phys:
            facts["phys"] = phys

    # --- lamps and buttons ----------------------------------------------
    holder = led_nodes(root)
    if holder is not None:
        keys_holder = holder.has_compatible(("gpio-keys", "gpio-keys-polled"))
        taken = set()
        for child in holder.children:
            gpio = first_gpio(root, child)
            if gpio is None:
                continue
            pin, low = gpio
            name = child.string("label") or child.name_plain()
            name = clean_name(name, taken)
            is_key = keys_holder \
                or child.string("default-state") == "input" \
                or "linux,code" in child.props
            if is_key:
                facts["keys"].append({"name": name, "pin": pin, "low": low,
                                      "code": child.cell("linux,code")})
            else:
                facts["leds"].append({"name": name, "pin": pin, "low": low})

    # --- the dies --------------------------------------------------------
    for n in [root] + list(root.walk()):
        if n.has_compatible(("m25p80", "jedec,spi-nor", "spi-nor")):
            facts["nor"] = True
        if n.has_compatible(("qcom,msm-nand", "qcom,ipq4019-nand",
                            "qcom,ipq40xx-nand")):
            facts["nand"] = True

    return facts


def bind_keys(facts, keymap):
    """Buttons become QEMU keys only when someone says which key they are.

    QEMU's input layer speaks qcodes (esc, f, f1, ...); a device tree speaks
    linux,code or nothing at all.  Guessing would make `sendkey esc` press a
    button the board calls something else, so an unbound button stays a pin and
    is reported.  A button is named by its label, by its pin number (two
    buttons can share a label in a tree that has none), or by its linux,code."""
    for k in facts["keys"]:
        q = keymap.get(k["name"])
        if q is None:
            q = keymap.get(str(k["pin"]))
        if q is None and k["code"] is not None:
            q = keymap.get(str(k["code"])) or keymap.get(hex(k["code"]))
        k["qcode"] = q
    return facts


def machine_props(facts, opts, warn):
    """The -machine property list, in the order the machine documents."""
    props = []

    def add(k, v):
        props.append("%s=%s" % (k, v))

    if opts.board:
        add("board", opts.board)
    if not opts.no_dies:
        add("nor", "on" if (opts.nor if opts.nor is not None
                            else facts["nor"]) else "off")
        if opts.nor_size:
            add("nor-size", opts.nor_size)
        add("nand", "on" if (opts.nand if opts.nand is not None
                             else facts["nand"]) else "off")
        if opts.nand_size:
            add("nand-size", opts.nand_size)
        if opts.boot:
            add("boot-device", opts.boot)
        if opts.nor_file:
            add("nor-drive", opts.nor_id)
        if opts.nand_file:
            add("nand-drive", opts.nand_id)

    if opts.eth_ports is not None:
        # an explicit list wins, including an empty one (no switch at all)
        add("eth-ports", opts.eth_ports)
    elif facts["phys"]:
        add("eth-ports", ";".join(facts["phys"]))
    if opts.eth_phy_id:
        add("eth-phy-id", opts.eth_phy_id)
    if opts.bridge_port is not None:
        add("eth-bridge-port", str(opts.bridge_port))

    lamps = ";".join(l["name"] + ":" + str(l["pin"]) + (":al" if l["low"] else "")
                     for l in facts["leds"])
    if lamps:
        add("leds", lamps)
    bound = [k for k in facts["keys"] if k.get("qcode")]
    unbound = [k for k in facts["keys"] if not k.get("qcode")]
    for k in unbound:
        warn("button %s (pin %u) has no QEMU key; bind it with --key %s=<key>"
             % (k["name"], k["pin"], k["pin"]))
    buttons = ";".join(k["name"] + ":" + str(k["pin"]) + (":al" if k["low"] else "")
                       + ":" + k["qcode"] for k in bound)
    if buttons:
        add("keys", buttons)
    if opts.extra:
        props.extend(opts.extra)
    return props


# --------------------------------------------------------------------------
# what a person needs: a libvirt domain, or a command line
# --------------------------------------------------------------------------

XML_HEADER = """\
<!--
  Generated by tools/dts2xml.py from {source}
  board model: {model}
  The board properties are real -machine properties; libvirt does not know this
  machine type, so they travel in the qemu:commandline escape hatch, and the
  dies travel as -drive if=none backends because libvirt has no way to say
  "a block backend with no device attached".
-->
"""


def xml_domain(props, opts, facts, source):
    lines = []
    add = lines.append
    add("<domain type='qemu' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>")
    add("  <name>%s</name>" % escape(opts.name))
    add("  <memory unit='MiB'>%d</memory>" % opts.memory)
    add("  <vcpu>%d</vcpu>" % opts.vcpus)
    add("  <os>")
    add("    <type arch='armv7l' machine='ipq4019'>hvm</type>")
    if opts.image:
        add("    <kernel>%s</kernel>" % escape(opts.image))
    add("    <boot dev='hd'/>")
    add("  </os>")
    add("  <devices>")
    add("    <emulator>%s</emulator>" % escape(opts.emulator))
    if opts.console:
        add("    <serial type='file'>")
        add("      <source path=%s/>" % quoteattr(opts.console))
        add("      <target port='0'/>")
        add("    </serial>")
    else:
        add("    <serial type='pty'>")
        add("      <target port='0'/>")
        add("    </serial>")
    add("  </devices>")
    add("  <qemu:commandline>")
    add("    <qemu:arg value='-machine'/>")
    add("    <qemu:arg value=%s/>" % quoteattr("ipq4019," + ",".join(props)))
    for die, path, ident in (("nor", opts.nor_file, opts.nor_id),
                             ("nand", opts.nand_file, opts.nand_id)):
        if not path:
            continue
        add("    <qemu:arg value='-drive'/>")
        add("    <qemu:arg value=%s/>"
            % quoteattr("if=none,id=%s,file=%s,format=raw"
                        % (ident, os.path.abspath(path))))
    add("  </qemu:commandline>")
    add("</domain>")
    return (XML_HEADER.format(source=source, model=facts["model"] or "unknown")
            + "\n".join(lines) + "\n")


def qemu_argv(props, opts, facts):
    args = [opts.emulator, "-M", "ipq4019," + ",".join(props)]
    for die, path, ident in (("nor", opts.nor_file, opts.nor_id),
                             ("nand", opts.nand_file, opts.nand_id)):
        if path:
            args += ["-drive", "if=none,id=%s,file=%s,format=raw"
                     % (ident, os.path.abspath(path))]
    if opts.image:
        args += ["-kernel", opts.image]
    if opts.console:
        args += ["-serial", "file:" + opts.console, "-display", "none"]
    else:
        args += ["-nographic"]
    return " ".join(shlex.quote(a) for a in args)


# --------------------------------------------------------------------------

def parse_keys(specs, warn):
    """--key name=qcode, and --key 0x1e=qcode for a linux,code."""
    out = {}
    for spec in specs or []:
        if "=" not in spec:
            warn("--key %s is not name=qcode; ignored" % spec)
            continue
        name, qcode = spec.split("=", 1)
        out[name] = qcode
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Turn a board device tree into this machine's "
                    "configuration (libvirt XML, argv, or -machine props).",
        epilog="The tree supplies population and polarity; sizes, key bindings "
               "and the boot device are decisions about the run, so they are "
               "options.")
    ap.add_argument("dtb", help="a .dtb (or .dts) to read")
    ap.add_argument("--print-machine", action="store_true",
                    help="print only the -machine property list")
    ap.add_argument("--print-argv", action="store_true",
                    help="print a plain qemu command line instead of XML")

    ap.add_argument("--name", default="ipq4019", help="libvirt domain name")
    ap.add_argument("--memory", type=int, default=256, help="MiB (default 256)")
    ap.add_argument("--vcpus", type=int, default=1)
    ap.add_argument("--emulator",
                    default=os.path.abspath("./build/qemu-system-arm"),
                    help="the QEMU binary the domain should run")
    ap.add_argument("--image", default=None,
                    help="flash image to program a blank die with (-kernel)")
    ap.add_argument("--console", default=None,
                    help="write the serial console to this file (XML) / use it "
                         "as -serial (argv)")

    ap.add_argument("--board", default=None,
                    help="override the board preset (default, none)")
    ap.add_argument("--nor", dest="nor", action="store_true", default=None,
                    help="force a NOR die even if the tree has none")
    ap.add_argument("--no-nor", dest="nor", action="store_false",
                    help="force no NOR die")
    ap.add_argument("--nand", dest="nand", action="store_true", default=None,
                    help="force a NAND die")
    ap.add_argument("--no-nand", dest="nand", action="store_false")
    ap.add_argument("--no-dies", action="store_true",
                    help="say nothing about dies at all (a pure SoC run)")
    ap.add_argument("--nor-size", default=None, help="die bytes, e.g. 32M")
    ap.add_argument("--nand-size", default=None, help="die bytes, e.g. 132M")
    ap.add_argument("--boot", default=None,
                    help="boot device: spi-nor, spi-nand, none")
    ap.add_argument("--nor-file", default=None,
                    help="the file that is the NOR part")
    ap.add_argument("--nand-file", default=None,
                    help="the file that is the NAND part")
    ap.add_argument("--nor-id", default="nor", help="block backend id for NOR")
    ap.add_argument("--nand-id", default="nand", help="block backend id for NAND")

    ap.add_argument("--eth-ports", default=None,
                    help="override the tree's MDIO population")
    ap.add_argument("--eth-phy-id", default=None, help="PHY identifier")
    ap.add_argument("--bridge-port", type=int, default=None,
                    help="switch port wired to the host (1..6)")

    ap.add_argument("--key", action="append", default=[],
                    metavar="NAME=QCODE",
                    help="bind a button to a QEMU key (e.g. button=esc); "
                         "may be repeated")
    ap.add_argument("--set", dest="extra", action="append", default=[],
                    metavar="PROP=VALUE",
                    help="append any other machine property verbatim")
    ap.add_argument("--quiet", action="store_true")

    opts = ap.parse_args(argv)

    def warn(msg):
        if not opts.quiet:
            print("dts2xml: %s" % msg, file=sys.stderr)

    try:
        root = load(opts.dtb)
    except (OSError, ValueError, Exception) as e:  # noqa: BLE001 - one message
        print("dts2xml: %s: %s" % (opts.dtb, e), file=sys.stderr)
        return 1

    facts = bind_keys(board_from_tree(root), parse_keys(opts.key, warn))
    props = machine_props(facts, opts, warn)

    if opts.print_machine:
        print("ipq4019," + ",".join(props))
    elif opts.print_argv:
        print(qemu_argv(props, opts, facts))
    else:
        sys.stdout.write(xml_domain(props, opts, facts, opts.dtb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
