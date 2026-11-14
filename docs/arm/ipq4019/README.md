# -M ipq4019 (Qualcomm IPQ4019)

ARMv7 (Cortex-A7), 32-bit ARM Linux.  The machine models the boot path from a
raw SPI-NOR dump and the peripherals a stock image needs: the QPIC NAND
controller, the ESS switch and its EDMA engine, the MDIO bus with copper PHYs,
the QUP SPI controller (with the BAM it uses), UART, GIC, timers and the
Qualcomm power/reset glue.

## Board presets

Selected with the `board` property:

| preset | content |
|---|---|
| `default` | 32 MiB SPI NOR, no NAND |
| `none` | bare SoC, nothing attached |
| `hap-ac2` | 32 MiB NOR, 132 MiB SPI NAND on CS1, five copper PHYs |
| `hap-ac3` | 16 MiB NOR and a 128 MiB parallel NAND behind the QPIC controller |

`hap-ac3` is the reference board for the boot chain and installation flows:

```
ipq4019: board hap-ac3: nor 16 MiB cs gpio 54, nand none, qpic-nand 132 MiB, boot-device spi-nor
ipq4019: qpic-nand 132 MiB, id c2 f1 80 1d, 65536 pages of 2048+64, 1024 blocks (RAM only)
```

By default a NAND is RAM only (a board that has never been installed).  Add
`qpic-nand-file=<die>` to make programming survive the machine exiting.

## Source layout

The machine is split by controller.  `ipq4019.c` is the machine itself
(memory map, reset, boot stages, `-M` properties, state dump); each block owns
its register file, its `MemoryRegionOps`, its state and one init entry point.
The sources are *copied* (they are not patched): `config.mk` maps each one to
the QEMU tree, the machine file to `hw/arm/ipq4019.c` and the blocks to
`hw/arm/ipq4019/`, so a block includes `"ipq4019.h"` and the hook series only
lists the block files in `meson.build`.

| file | owns |
|---|---|
| `ipq4019_internal.h` | SoC/board state, base addresses, the interface between blocks |
| `ipq4019.c` | machine init, memory map, reset, boot stages, properties, state dump |
| `modules/gpio.c` | TLMM pin controller, LED panel, keys |
| `modules/uart.c` | BLSP UARTDM console |
| `modules/bam.c` | BLSP1 BAM (the QUP FIFOs' DMA engine) |
| `modules/eth.c` | IPQESS, host netdev, MDIO/PHY, board ports |

Planned, in the order they are being moved: `modules/stub.c` (shadowed
stubs, power-hold, watchdog, memory window, gcnt), `modules/flash.c` (QUP SPI,
NOR die, die store, SPI NAND), `modules/qpic.c` + `modules/qpic_bam.c`,
`modules/board.c` (presets, config merge, env shim), then `modules/dump.c` for
what is left.  `modules/flash.c` is deliberately later: the QUP SPI opcode
handler sits with the part-building code, so it is a multi-range move.

Rules that keep the split useful:

- a block's state struct lives in `ipq4019_internal.h` and its code in the
  block's file -- the state dump reads every block, so hiding the structs
  would only move the coupling;
- anything another file needs is a prototype in the header; nothing else
  crosses a file boundary, and the header stays small;
- adding a block is a `config.mk` entry (`ipq4019_MACHINE_SRCS`), a file in
  the machine patch's `meson.build` hunk, and a line in the header -- nothing
  else.

## Building

The machine is built with the `arm` target, which produces
`qemu-system-arm`.  It is selected like every other target — in `.config` or
on the command line:

```sh
make list                     # [x] arm/ipq4019  (wired)
make TARGET=arm/ipq4019 build # fetch → prep → configure → ninja
make TARGET=arm build         # same, spelled as the whole architecture
```

The binary is symlinked at `build/arm/qemu-system-arm`; the sources it was
built from are a git tree under `build/arm/src`.

The only QEMU-side change this machine needs is `0001-arm-ipq4019-machine-support-hooks.patch`,
kept in the machine's own series (it is not a generic ARM change).
`make TARGET=arm/ipq4019 check-patches` proves it still applies to the pinned
release.

## Running

```sh
build/arm/qemu-system-arm \
  -M "ipq4019,board=hap-ac3,eth-bridge-port=1,qpic-nand-file=/tmp/die.bin" \
  -kernel artifacts/<nor-image>.bin -display none -monitor none -serial stdio \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no
```

All images are supplied by the user; the machine never modifies the file named
by `-kernel`.  A die named with `qpic-nand-file=` (or `nor-file=`) is the part
itself: creation is once, then every program and erase is written through.

The tap is only needed for the networking flows (`eth-bridge-port` selects
which switch port is wired to it, see the ESS section in `model-quirks.md`);
serial-only bring-up runs without `-netdev`.

## Documentation

- `model-quirks.md` — emulator gaps and deliberate deviations, including the
  boot-chain handover and what is not modelled.
- `boot-and-storage.md` — the boot stages, the die model and the QPIC NAND
  layout.

## Tests

`tests/arm/ipq4019/` holds payload sources (`payloads/`) and scenario scripts
(`scenarios/`).  The scenarios drive the built binary:

```sh
tests/arm/ipq4019/scenarios/board-matrix.sh
```
