# NAND images: flat dump, full die, and persistence

## Two layouts, one chip

The machine accepts either storage layout, chosen from the file size by
`mt7621_nand_load` (real NAND always has a power-of-two page count):

| layout | bytes per page | 128 MiB chip | notes |
|---|---|---|---|
| flat dump | 2048 | 128 MiB | main area only; the common dump format |
| full die | 2048 + 64 | 132 MiB | page + spare |

The model prints which one it picked:

```
mt7621: NAND dump includes the spare area (2112-byte pages, 65536 pages)
```

Page reads hand the guest page + spare and honour the column address, so a
die image lets the guest see real OOB content (bad-block markers, ECC bytes).
The ECC engines report "clean" (encoder/decoder idle, zero corrected or
uncorrectable bits), so stored ECC is never validated — a die image is about
preserving OOB, not about corrupting reads.

## Converting

`tools/mipsel/mt7621/nand-die.py`:

```sh
tools/mipsel/mt7621/nand-die.py info image.bin
#   132.0 MiB, layout: die, 65536 pages, 1024 erase blocks, bad blocks: 0

tools/mipsel/mt7621/nand-die.py die2dump image.bin data.bin
tools/mipsel/mt7621/nand-die.py die2dump image.bin data.bin --oob oob.bin
tools/mipsel/mt7621/nand-die.py dump2die data.bin rebuilt.bin --oob-from image.bin
```

`dump2die` without `--oob-from` fills the spare area with `0xff` (erased, i.e.
"not bad").  A flat dump has no OOB, so markers can only come from a die
image; `dump2die … --oob-from <die>` round-trips byte-identically.

## Persisting guest writes

By default the guest runs against a private copy in RAM and the boot image
stays pristine.  Two ways to make writes survive a restart:

```sh
# let the machine create and seed the state file itself
MT7621_NAND_IMAGE=~/nand-state.img qemu-system-mipsel -M mt7621,… -kernel dump

# or a normal QEMU block device
qemu-system-mipsel -M mt7621,… -drive file=nand-state.img,if=none,id=nand,format=raw
```

First use seeds the file from the boot dump; afterwards the file *is* the
flash and page programs (`0x10`) and block erases (`0xd0`) are written through
immediately.  Both layouts work; the state file is as large as the boot image.

If the file cannot be opened read-write the machine warns and keeps changes in
RAM only.  Note that a die state file seeded from a flat dump starts with an
erased spare area, so the guest's bad-block scan sees a clean chip and writes
its own markers into the state file.
