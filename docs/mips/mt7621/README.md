# -M mt7621 (MediaTek MT7621A)

MIPS 1004Kc, 256 MiB DDR, the SoC peripherals at `0x1e000000` (system
controller, UARTs, SPI/NAND, I2C, watchdog), the GIC at `0x1fbc0000`, the
frame engine with PDMA/QDMA rings and the MT7530 switch behind MDIO.

## Board presets

Selected with the `board` property:

| preset | boot flash | notes |
|---|---|---|
| `lg-gapm7200` | parallel NAND (uImage bootloader at offset 0x4000) | default |
| `rb750gr3` | SPI NOR (memory-mapped boot ROM window) | |

`MT7621_*` environment knobs exist for A/B work; see `model-quirks.md`.

## Running

```sh
build/mips/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
  -kernel <dump> -display none -serial stdio \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no
```

The flash image is a raw dump supplied by the user; the machine never modifies
it (`MT7621_NAND_IMAGE` gives a persistent copy, see `nand-images.md`).


## Tests

```sh
tests/mips/mt7621/boot-smoke.sh <dump>
tests/mips/mt7621/net-ping.sh <dump> <guest-ip>
```

## Documentation

- `model-quirks.md` — emulator gaps and the deliberate deviations.
- `nand-images.md` — flat dumps vs full-die images and persistence.
