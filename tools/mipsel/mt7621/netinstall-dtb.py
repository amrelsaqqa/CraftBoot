#!/usr/bin/env python3
"""Patch the catch-all DTB embedded inside MikroTik's netinstall-cli binary.

The binary carries one flat FDT (no appended-DTB ELF here, unlike
`dt-patch.py` which edits netinboot).  It is MikroTik's catch-all tree: every
board's nodes are present, per-board ones marked `qdisabled`/`status`.

Edits applied (and nothing else):
  * `palmbus@1e000000/uartlite@c00` -> status = "okay"   (RB750Gr3 console)
  * `/chosen` bootargs -> console=ttyS0,115200 earlycon=... loglevel=8
  * LED subtrees (`*-leds`, `leds`, `gpio-leds`) deleted to free the space the
    longer bootargs needs

totalsize is kept byte-for-byte identical to the original blob (10101), and
the binary's file size is unchanged, so no other offsets in the file move and
the CLI still serves the DTB it expects.  Surplus structure space is filled
with FDT_NOP tokens; any remainder is zero padding at the end of the blob.
"""
import argparse
import struct

TOK_BEGIN, TOK_END_NODE, TOK_PROP, TOK_NOP, TOK_END = 1, 2, 3, 4, 9
MAX_ITEMS = 100000

BOOTARGS = ("console=ttyS0,115200 earlycon=uart8250,mmio32,0x1e000c00 "
            "loglevel=8")
# Appended verbatim by --bootargs, e.g. "yaffs.inband_tags=1".  The blob keeps
# its totalsize, so only the freed LED space limits how long this can get.


def parse(blob):
    (magic, total, off_struct, off_strings, off_rsv, ver, lastc, cpuid,
     size_str, size_struct) = struct.unpack_from(">10I", blob, 0)
    if magic != 0xD00DFEED:
        raise SystemExit("not an FDT at that offset (magic %#x)" % magic)
    strings = blob[off_strings:off_strings + size_str]

    def s_at(o):
        e = strings.index(b"\x00", o)
        return strings[o:e].decode("ascii")

    root, stack = None, []
    pos, end, seen = off_struct, off_struct + size_struct, 0
    while pos + 4 <= end:
        seen += 1
        if seen > MAX_ITEMS:
            raise SystemExit("structure walk did not terminate")
        tok = struct.unpack_from(">I", blob, pos)[0]
        if tok == TOK_BEGIN:
            nul = blob.index(b"\x00", pos + 4)
            node = {"name": blob[pos + 4:nul].decode("ascii"), "items": []}
            if stack:
                stack[-1]["items"].append(("node", node))
            else:
                root = node
            stack.append(node)
            pos += 4 + (nul - (pos + 4) + 1 + 3) // 4 * 4
        elif tok == TOK_END_NODE:
            stack.pop()
            pos += 4
        elif tok == TOK_PROP:
            ln, no = struct.unpack_from(">II", blob, pos + 4)
            val = blob[pos + 12:pos + 12 + ln]
            stack[-1]["items"].append(("prop", (s_at(no), val)))
            pos += 12 + (ln + 3) // 4 * 4
        elif tok == TOK_NOP:
            pos += 4
        elif tok == TOK_END:
            break
        else:
            raise SystemExit("bad token %#x at rel %#x" % (tok, pos))
    if stack:
        raise SystemExit("unbalanced structure block")
    hdr = dict(total=total, off_struct=off_struct, off_rsv=off_rsv, ver=ver,
               lastc=lastc, cpuid=cpuid, size_str=size_str,
               size_struct=size_struct)
    return root, hdr


def walk(node, depth=0):
    yield node, depth
    for kind, item in node["items"]:
        if kind == "node":
            yield from walk(item, depth + 1)


def get_prop(node, name):
    for kind, item in node["items"]:
        if kind == "prop" and item[0] == name:
            return item[1]
    return None


def set_prop(node, name, val):
    for i, (kind, item) in enumerate(node["items"]):
        if kind == "prop" and item[0] == name:
            node["items"][i] = ("prop", (name, val))
            return
    node["items"].append(("prop", (name, val)))


def del_nodes(node, pred):
    removed, keep = [], []
    for kind, item in node["items"]:
        if kind == "node":
            if pred(item["name"]):
                removed.append(item["name"])
                continue
            removed += del_nodes(item, pred)
        keep.append((kind, item))
    node["items"] = keep
    return removed


def collect_strings(node, order):
    for kind, item in node["items"]:
        if kind == "prop":
            order.setdefault(item[0], None)
        else:
            collect_strings(item, order)


def serialize(node, stroff):
    out = bytearray(struct.pack(">I", TOK_BEGIN))
    nb = node["name"].encode("ascii") + b"\x00"
    out += nb + b"\x00" * (-len(nb) % 4)
    for kind, item in node["items"]:
        if kind == "prop":
            name, val = item
            out += struct.pack(">III", TOK_PROP, len(val), stroff[name])
            out += val + b"\x00" * (-len(val) % 4)
        else:
            out += serialize(item, stroff)
    return bytes(out + struct.pack(">I", TOK_END_NODE))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", nargs="?",
                    default="/home/amrelsaqqa/Downloads/netinstall-7.15/netinstall-cli")
    ap.add_argument("dst", nargs="?",
                    default="/tmp/netinstall-cli-dtb")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0x954Aae,
                    help="file offset of the FDT magic (default 0x954aae)")
    ap.add_argument("--total", type=lambda s: int(s, 0), default=10101,
                    help="FDT totalsize to preserve (default 10101)")
    ap.add_argument("--tree", action="store_true", help="dump the node tree")
    ap.add_argument("--swap-parts", action="store_true",
                    help="swap the RouterOS/RouterBoot partition nodes, so the "
                         "kernel registers RouterBoot as mtd0 and RouterOS as "
                         "mtd1 (what the netinstall agent's /dev/mtdBoot=31,0 and "
                         "/dev/mtdRoot=31,1 assume)")
    ap.add_argument("--bootargs", default=BOOTARGS,
                    help="value for /chosen/bootargs (default: console only)")
    args = ap.parse_args()

    data = open(args.src, "rb").read()
    base, total = args.offset, args.total
    blob = data[base:base + total]
    root, hdr = parse(blob)
    if hdr["total"] != total:
        raise SystemExit("blob totalsize is %d, expected %d"
                         % (hdr["total"], total))
    print("DTB @%#x totalsize %d struct@%#x/%#x strings@%#x/%#x" %
          (base, total, hdr["off_struct"], hdr["size_struct"],
           hdr["off_struct"] + hdr["size_struct"], hdr["size_str"]))

    if args.tree:
        for n, d in walk(root):
            st = get_prop(n, "status")
            st = st.rstrip(b"\x00").decode() if st else ""
            print("%s%s%s" % ("  " * d, n["name"], "  [" + st + "]" if st else ""))

    uart = next((n for n, _ in walk(root) if n["name"] == "uartlite@c00"), None)
    if uart is None:
        raise SystemExit("uartlite@c00 not found")
    set_prop(uart, "status", b"okay\x00")
    print("uartlite@c00 status -> okay")

    chosen = next((n for n, d in walk(root) if n["name"] == "chosen" and d == 1), None)
    if chosen is None:
        chosen = {"name": "chosen", "items": []}
        root["items"].append(("node", chosen))
        print("added /chosen")
    set_prop(chosen, "bootargs", args.bootargs.encode() + b"\x00")
    print("chosen bootargs -> %s" % args.bootargs)

    if args.swap_parts:
        for node, _d in walk(root):
            parts = [it for kind, it in node["items"]
                     if kind == "node" and it["name"].startswith("partition@")]
            if len(parts) >= 2:
                others = [(kind, it) for kind, it in node["items"]
                          if not (kind == "node" and
                                  it["name"].startswith("partition@"))]
                node["items"] = others + [("node", p)
                                          for p in reversed(parts)]
                print("swap-parts: %s -> %s" %
                      (node["name"], [p["name"] for p in reversed(parts)]))

    removed = del_nodes(root, lambda nm: nm.endswith("-leds") or
                        nm in ("leds", "gpio-leds", "led-controller"))
    print("removed LED nodes: %s" % (removed or "none"))

    order = {}
    collect_strings(root, order)
    strtab = bytearray()
    for name in order:
        order[name] = len(strtab)
        strtab += name.encode("ascii") + b"\x00"
    struct_blk = serialize(root, order)

    avail = total - hdr["off_struct"] - len(strtab)
    if len(struct_blk) > avail:
        raise SystemExit("does not fit: struct needs %d B, %d B free "
                         "(delete more nodes)" % (len(struct_blk), avail))
    size_struct = avail // 4 * 4
    struct_blk += struct.pack(">I", TOK_NOP) * ((size_struct - len(struct_blk)) // 4)
    tail = total - hdr["off_struct"] - size_struct - len(strtab)

    new = bytearray(total)
    new[:hdr["off_struct"]] = blob[:hdr["off_struct"]]
    new[hdr["off_struct"]:hdr["off_struct"] + size_struct] = struct_blk
    new[hdr["off_struct"] + size_struct:
        hdr["off_struct"] + size_struct + len(strtab)] = strtab
    struct.pack_into(">10I", new, 0, 0xD00DFEED, total, hdr["off_struct"],
                     hdr["off_struct"] + size_struct, hdr["off_rsv"],
                     hdr["ver"], hdr["lastc"], hdr["cpuid"], len(strtab),
                     size_struct)

    # re-parse the rebuilt blob before touching the binary
    root2, hdr2 = parse(bytes(new))
    assert hdr2["total"] == total and len(new) == total
    st = next((get_prop(n, "status") for n, _ in walk(root2)
               if n["name"] == "uartlite@c00"), None)
    ba = next((get_prop(n, "bootargs") for n, d in walk(root2)
               if n["name"] == "chosen" and d == 1), None)
    led = [n["name"] for n, _ in walk(root2) if "led" in n["name"]]
    print("verify: uart=%r bootargs=%r leds=%s" %
          (st.rstrip(b"\x00").decode() if st else None,
           ba.rstrip(b"\x00").decode() if ba else None, led or "none"))
    print("layout: size_struct=%#x size_strings=%#x tail_pad=%d" %
          (hdr2["size_struct"], hdr2["size_str"], tail))

    out = bytearray(data)
    out[base:base + total] = new
    open(args.dst, "wb").write(out)
    print("written: %s (%d B, original %d B, same size: %s)" %
          (args.dst, len(out), len(data), len(out) == len(data)))


if __name__ == "__main__":
    main()
