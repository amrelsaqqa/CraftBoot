# ipq4019 — boot chain and die storage

## Boot device

`boot-device` says which die the boot chain is read from:

| value | the first stage comes from |
|---|---|
| `spi-nor` | the SPI NOR on chip select 0 (default) |
| `spi-nand` | the SPI NAND on chip select 1 |
| `qpic-nand` | the parallel NAND behind the QPIC controller |

For a NAND the machine hands the boot chain the page data with the spare area
removed, which is what the ROM's NAND path would present.  A boot strap that
names a device the board does not have is refused at machine start, and
`-kernel` is content for the boot device — it does not create one.

## Dies are devices

A die takes its storage in one of two forms, because there are two worlds to fit
into:

| property | storage |
|---|---|
| `nor-file=`, `nand-file=`, `qpic-nand-file=` | a path the machine opens |
| `nor-drive=`, `nand-drive=`, `qpic-nand-drive=` | a block backend somebody else opened |

The second is what a libvirt `<disk>` produces: libvirt owns the file, its
format and its cache mode, and hands QEMU a backend by name.

How the machine treats a store:

* **Empty** (`: > die.img`, or `qemu-img create -f raw die.img 0`) is a blank
  part: it is sized to the die and programmed from the image.
* **Zero-filled** is treated the same way only when an image is named to
  program it with, because a real unprogrammed part reads `0xff`.
* **Already holding a die** is the part: nothing is programmed over it, the
  image is not read, and every erase and programmed byte is mirrored into it.
  Killing and restarting QEMU is then a power cycle.
* **Not writable** is a write-protected part: the guest's program and erase
  fail the way the status register reports them instead of the machine
  refusing to start.

A NAND die carries the spare area, so its length is `pages * (page + spare)`
exactly; any other length is a different part and is refused rather than
extended.

## QPIC NAND layout

The part behind the QPIC controller is a 2 KiB + 64 B SLC NAND: 128 KiB erase
blocks, 1024 of them on a 128 MiB die.

* **Codeword framing** follows the controller's `CFG0` codeword-size field, not
  a fixed stride.  A guest that programs with 516-byte data codewords (ECC
  inline) and one that programs raw 528-byte frames therefore each read back
  exactly what they wrote; getting this wrong made written filesystems
  unreadable to the next boot stage.
* **Spare bytes** are addressable through the controller's registers, so a
  guest that stores tags there (in-band-tags filesystems) and a guest that
  leaves the area to the controller both see the layout they expect.
* **Bad-block markers** (the first two spare bytes) are preserved on codeword
  programs: they belong to the part, not to the guest.
* **Bad blocks** are marked by those bytes alone at format time.  A guest
  that formats a die writes the erase counters and volume tables itself; the
  machine does not invent markers a real part would not have.

Writing a codeword re-encodes the data bytes into the model's frame layout, so
a die written by this machine can be read by the real controller semantics
rather than by a mirror of the guest's buffer.

## Filesystems seen through it

Two layouts have been exercised end to end:

* a yaffs filesystem in the boot partition (a bootloader that reads its kernel
  file through yaffs), with in-band tags enabled in the command line;
* a UBI volume table in the main partition (a Linux installation), formatted by
  the guest itself with `ubiformat`, then filled by a sysupgrade.

Both are written by the guest through the normal NAND commands; the machine
never pre-formats them.
