# CraftBoot

**QEMU anything.**

Run router firmware on your laptop instead of on the router.

This repository holds the **machine models** (emulated SoCs) and the small set
of patches they need in QEMU, packaged so that a single `make` produces a
working `qemu-system-*` binary for a given board.  You supply the firmware
image; the project never ships one.

It is aimed at bring-up and debugging work: watch the boot chain over serial,
attach a debugger, bridge an Ethernet port to your host, bisect a change
without bricking hardware, and keep regression tests for all of it.

---

## Demo

<p align="center">
  <video src="https://github.com/user-attachments/assets/cd33f09e-8f5f-465f-92eb-24c49e0d90f4" width="960" controls playsinline></video>
</p>

The `arm/ipq4019` machine booting OpenWrt 24.10 from a 32 MB NOR dump, with one
switch port bridged to a host tap: U-Boot, then the kernel bringing up its DSA
switch on `ipqess-edma`, then userland.  Two minutes, real time, no edits.

If the player does not load,
[open the recording directly](https://github.com/user-attachments/assets/cd33f09e-8f5f-465f-92eb-24c49e0d90f4).

---

## What this is

- **Machine models for router SoCs.**  Each model emulates the CPU, DRAM
  controller, interrupt controller, timers, UART, flash/NAND controllers and
  Ethernet/switch blocks a firmware needs to boot: enough to run the real boot
  chain and kernel, not a functional replica of every peripheral.
- **A reproducible build recipe.**  The QEMU source is *not* stored here.  Each
  target pins a QEMU release, downloads it once into a shared cache, applies an
  ordered patch series in a throw-away tree, and builds.
- **A test surface.**  Boot smoke tests and network tests that use the built
  binary, so a change to a model can be checked in one command.

## What this is not

- Not a firmware image collection.  You provide the dump (NOR/NAND/eMMC) of a
  device you own; nothing is redistributed here.
- Not a full-system simulator of the hardware.  Peripherals beyond the boot and
  debug path are stubs or absent, and the gaps are documented per machine.
- Not a fork of QEMU.  The changes are a patch series against a pinned release.

---

## Supported machines

A **target** builds one executable (`qemu-system-<arch>`); a **machine** is a
`-M` type inside it; a **board** is a machine preset chosen at run time.

| target | machine | boards (`-M <machine>,board=…`) | notes |
|---|---|---|---|
| `mips` | `mt7621` | `lg-gapm7200` (default), `rb750gr3` | MIPS 1004Kc, DDR, MT7530 switch |
| `arm` | `ipq4019` | `hap-ac2` (default) | ARMv7 Cortex-A7, QPIC NAND, ESS switch |

Adding another SoC of a covered architecture (for example MT7628AN) means
adding a *machine* to the `mipsel` target — no new target, no new executable.

---

## How the build works

```
        .config / TARGET=…
                │
        ┌───────▼────────┐
        │ fetch          │  git clone --depth 1 --branch <tag>  →  ~/.cache/qemu-src/qemu-<tag>
        ├────────────────┤
        │ prep           │  extract tag → build/<target>/src
        │                │  git init + baseline commit
        │                │  copy machines/<soc>/<soc>.c
        │                │  git am: generic → target → machine series
        ├────────────────┤
        │ build          │  configure --target-list=… ; ninja qemu-system-<arch>
        └───────┬────────┘
                │
     build/<target>/qemu-system-<arch>   (symlink to the binary)
```

Everything under `build/` is disposable.  The download cache is shared between
targets and keyed by tag, so two targets can pin different QEMU releases
without interfering.

---

## Repository layout

```
COPYING                   license: GPL-2.0 (code is GPL-2.0-or-later)
config.mk                 QEMU repo/tag defaults, cache location, configure args
Makefile                  fetch / prep / build / check-patches / export-patches / clean
.config                   which targets and machines are built by default
scripts/selected.sh       resolves "mipsel/mt7621,arm/*" into build groups
patches/generic/          patches that must apply to every target
targets/<arch>/
  config.mk               target-list, binaries, machine list
  series                  target-wide patches (ordered)
  patches/
  machines/<soc>/
    <soc>.c               the machine model — copied into the tree, never patched
    config.mk             where the source goes, board presets, dev branch
    series                machine-only patches (optional)
docs/<arch>/<soc>/        machine documentation and model quirks
tools/<arch>/<soc>/       converters, probes, console drivers
tests/<arch>/<soc>/       boot and network checks
artifacts/                large dumps and images (gitignored, kept on disk)
```

---

## Requirements

- Linux (the tests use `tap` and `ping`; the build itself is portable).
- `git`, `python3`, `ninja`, a C toolchain, `pkg-config`, and QEMU's usual
  development packages (`glib2`, `pixman`, `zlib`; the distro package names
  differ, e.g. `glib2-devel pixman-devel zlib-devel`).
- ~2 GB free for one build tree, plus the cached QEMU source (~150 MB).
- Network access the first time a QEMU release is fetched (and for meson, if
  your distro does not ship a new enough one).

---

## Getting started

### 1. See what can be built

```sh
make list
# [x] arm/ipq4019  (wired)
# [x] mipsel/mt7621  (wired)
```

`x` marks what the current `.config` selects; `wired` means the target has a
binary and a machine list, so it can actually be built.  Edit `.config` to
change the default selection, or run `make defconfig` to regenerate it for
every wired target.

```
# .config
CONFIG_TARGET_mipsel=y
CONFIG_MACHINE_mt7621=y
CONFIG_TARGET_arm=y
CONFIG_MACHINE_ipq4019=y
```

Command-line selection always wins over `.config`:

```sh
make TARGET=mipsel                      # every machine of mipsel
make TARGET=mipsel/mt7621               # one machine
make TARGET=mipsel/mt7621,mipsel/mt7622   # an explicit set
make TARGET='mipsel/*,arm/*'            # whole architectures (quote the *)
```

Machines of the same target share one tree and one executable: requesting
`mipsel/mt7621,mipsel/mt7622` builds a single `qemu-system-mipsel` containing both.

### 2. Validate the patches (fast, no build)

```sh
make TARGET=mipsel check-patches
# series applies cleanly: mipsel (mt7621) on v10.2.2
```

This extracts the pinned QEMU tag into a temporary tree, applies every series
in order with `git am -3`, and throws the tree away.  It is the first thing to
run after changing anything, and the fastest way to notice that a patch no
longer matches the pinned release.

### 3. Build

```sh
make TARGET=mipsel build        # fetch → prep → configure → ninja
```

The result is symlinked next to the tree:

```
build/mipsel/qemu-system-mipsel
build/mipsel/src        # QEMU tag + machine source + your patches, as commits
build/mipsel/build      # configure/ninja output
```

`build/mipsel/src` is a normal git repository, so you can read `git log` there
to see exactly what was applied.

### 4. Run

```sh
build/mipsel/qemu-system-mipsel \
  -M mt7621,board=lg-gapm7200 -m 256M \
  -kernel ~/my-device-dump.bin \
  -display none -serial stdio
```

The firmware's own console appears on the serial line.  For the ARM machine:

```sh
make TARGET=arm build
build/arm/qemu-system-arm -M ipq4019,board=hap-ac2 -m 256M \
  -kernel ~/my-device-dump.bin -display none -serial stdio
```

### 5. Networking (optional)

The models bridge one switch port to a host tap device, so the guest gets a
real L2 link:

```sh
sudo ip tuntap add dev tap0 mode tap user "$USER"
sudo ip link set tap0 up
sudo ip addr add 192.168.33.2/24 dev tap0     # match the guest's subnet

build/mipsel/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
  -kernel ~/my-device-dump.bin -display none -serial stdio \
  -netdev tap,id=net0,ifname=tap0,script=no,downscript=no
```

Then ping the guest from the host once its bridge is up.  Which port is
bridged, and which address the firmware uses, depend on the board preset and
the firmware; see the machine documentation.

---

## Housekeeping

```sh
make TARGET=mipsel clean      # remove build/ for every target
make distclean              # remove build/ and the QEMU download cache
```

`clean` keeps the cached QEMU source; `distclean` forces a fresh fetch.  If a
build ever looks impossible, `make distclean && make TARGET=… build` is the
guaranteed reset.

---

## Working on a model

Machine sources are **copied** into the build tree, not patched: they change
wholesale and would bury the hook differences.  Everything else (the few
hundred lines of core changes) is an ordered patch series.

The series is generated from a normal git branch, so history and `git bisect`
stay available:

```sh
# in a development checkout of QEMU, on a branch based on the pinned tag
git commit -am "mt7621: …"

make TARGET=mipsel export-patches DEV=~/dev/qemu     # rewrite patches/ + series
make TARGET=mipsel check-patches prep                # validate, rebuild the tree
```

`export-patches` re-generates the target's patches from the branch, refreshes
the machine source, and rewrites `series`.  `prep` replays the result into
`build/<target>/src` as commits.

Patch order matters: `patches/generic/` (when it has a `series`) first, then
`targets/<arch>/series`, then each selected machine's `series`.  A patch belongs in `patches/generic/`
only if `make TARGETS="…" check-patches` passes for every target.

---

## Tests

Checks live next to the machines and use the built binary:

```sh
tests/mipsel/mt7621/boot-smoke.sh <dump>                 # boot, check expected lines
tests/mipsel/mt7621/net-ping.sh <dump> <guest-ip>        # boot with a tap, ping
```

`boot-smoke.sh` takes the image and a timeout; expected console patterns can be
overridden with `BOOT_EXPECT` (newline-separated regexes).  `net-ping.sh`
accepts an optional ready-regex and uses `TAP` (default `tap0`).

ARM scenario scripts live in `tests/arm/ipq4019/scenarios/` (board matrix,
libvirt spike, panel/VNC helpers) with payload sources in `payloads/`.

---

## Contributing

The most useful contribution is a firmware image this project has never booted.
Every machine here was written against a handful of dumps; a different build, a
different board revision or a different vendor is what exposes the register that
is still missing.

### Test a machine with your own dump

No code required:

```sh
make TARGET=mipsel/mt7621 build
tests/mipsel/mt7621/boot-smoke.sh ~/my-dump.bin
```

Watch it by hand instead, which shows every line the firmware prints:

```sh
build/mipsel/qemu-system-mipsel -M mt7621,board=lg-gapm7200 -m 256M \
  -kernel ~/my-dump.bin -display none -serial stdio
```

and for the ARM machine:

```sh
make TARGET=arm/ipq4019 build
build/arm/qemu-system-arm -M ipq4019,board=hap-ac2 -m 256M \
  -kernel ~/my-dump.bin -display none -serial stdio
```

A whole-chip dump (bootloader, partition table, everything) is more useful than
a kernel image: the models start at the boot ROM / bootloader step.
`docs/mipsel/mt7621/nand-images.md` describes the dump shapes the MT7621 machine
accepts, and each `docs/<arch>/<soc>/model-quirks.md` lists what that model
knows it does not implement — worth reading before treating a hang as a bug.

### Report what you see

An issue that can be reproduced gets fixed.  Include:

1. the exact command line, board preset included;
2. where it stops — the last serial lines, and anything QEMU printed itself (a
   QEMU-level error usually means the image format was rejected, not that the
   firmware failed);
3. what the image is: vendor, model, firmware version, and how it was dumped
   (whole chip, bootloader only, kernel only);
4. whether the dump can be shared.  If it cannot be published, attach the
   console log plus whatever else you can (partition table, `strings` of the
   bootloader, memory map) and say what is missing.

Dumps are large and vendor-copyrighted: never attach them to the repository —
link to or describe them in the issue.

### Add a machine or a board

An SoC in an architecture that is already covered is a **machine**, not a new
target:

```
targets/<arch>/machines/<soc>/
  <soc>.c      the model: TypeInfo, memory map, boot path, device wiring
  config.mk    <soc>_MACHINE_DST (path inside the QEMU tree),
               <soc>_BOARDS (runtime presets), <soc>_DEV_BRANCH
  series       machine-only patches, if the SoC needs its own hooks
```

Add `<soc>` to `<arch>_MACHINES` in `targets/<arch>/config.mk`, then:

```sh
make TARGET=<arch>/<soc> check-patches prep build
make list
```

`check-patches` must pass against the pinned tag before a series is acceptable.

A **board** is a preset of an existing machine (`-M mt7621,board=…`), so it is
usually an entry in `<soc>_BOARDS` plus whatever differs at reset: flash layout,
switch port mapping, DRAM size, MAC addresses.

A new **architecture** is a new `targets/<arch>/config.mk` naming
`<arch>_TARGET_LIST`, `<arch>_BINARIES` and `<arch>_MACHINES` — the only place
where a new `qemu-system-*` executable appears.

Patch series are generated from a branch, never hand-edited:

```sh
make TARGET=<arch> export-patches DEV=~/dev/qemu   # format-patch from DEV_BRANCH
make TARGET=<arch> check-patches                   # must apply to the pinned tag
```

Keep changes as separate, reviewable commits: they arrive in
`build/<target>/src` as commits, so `git log` there documents exactly what was
applied and `git bisect` works.

New machines should come with `docs/<arch>/<soc>/README.md` (what it models,
which presets exist, known limits) and, where it can run unattended, a check
under `tests/<arch>/<soc>/`.

### Ground rules

- **Never patch a guest image.**  The models run firmware as it is; a fix that
  requires modifying the dump is not a fix.
- **No dumps, images or vendor binaries in the repository** — they are
  copyrighted by their vendors and are kept out by `.gitignore`.
- Contributions are accepted under `GPL-2.0-or-later`, as is the rest of the
  code.

---

## Configuration reference

| setting | where | meaning |
|---|---|---|
| `TARGET`, `TARGETS` | command line | selection, see above |
| `MACHINES` | command line | override the machine list of one target |
| `QEMU_TAG_DEFAULT` | `config.mk` | QEMU release used when a target does not pin one |
| `<target>_QEMU_TAG` | `targets/<arch>/config.mk` | per-target release pin |
| `QEMU_REPO` | `config.mk` | where the source is fetched from |
| `QEMU_CACHE` | `config.mk` / env | download cache (one directory per tag) |
| `CONFIGURE_ARGS` | `config.mk` | extra configure flags for every target |
| `<target>_CONFIGURE` | `targets/<arch>/config.mk` | extra configure flags for one target |
| `DEV`, `DEV_BRANCH` | command line / target config | source checkout for `export-patches` |

Each machine documents its own run-time knobs in
`docs/<arch>/<soc>/model-quirks.md` (for example `MT7621_NAND_IMAGE` for a
persistent flash image).

---

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `series FAILED at …` | a patch no longer applies to the pinned tag; rebase it in the dev branch and re-export |
| `target … is not wired up` | the target has no `BINARIES`/machines yet — see its `config.mk` |
| `selected.sh: unknown machine …` | typo, or the machine does not exist under `targets/<arch>/machines/` |
| fetch fails / stale cache | `make distclean`, then build again |
| `TARGET=mipsel/*` behaves oddly | your shell expanded the `*`; quote it: `make TARGET='mips/*'` |
| tap test says unreachable | tap down, missing host address, or the guest uses a different subnet — check the machine docs |
| guest boots but no network | only one switch port is bridged; the active port depends on the board preset |

---

## Documentation

- `docs/mipsel/mt7621/README.md` — the MT7621 machine and its presets.
- `docs/mipsel/mt7621/model-quirks.md` — where the model deliberately differs.
- `docs/mipsel/mt7621/nand-images.md` — flat dumps vs full-die images, persistence.
- `docs/arm/ipq4019/README.md` — the IPQ4019 machine and its status.

---

## Scope and licensing

The code you find here is **GPL-2.0-or-later**; the full text is in `COPYING`.
The patch series is the exception: a patch carries the license of the QEMU file
it modifies, and QEMU mixes GPL-2.0, GPL-2.0-or-later and LGPL-2.1-or-later code
that the series touches, so the patches are not stamped with one identifier.
They are applied to QEMU and distributed as part of it, which makes the
resulting `qemu-system-*` binary GPLv2.

| part | license |
|---|---|
| machine models, tools, tests, build files | `GPL-2.0-or-later` |
| `targets/*/patches/`, `targets/*/machines/*/patches/` | the license of each modified QEMU file |
| documentation (`README.md`, `docs/`) | `GPL-2.0-or-later` |

Firmware images are **not** part of this repository and are never modified by
the emulator: you provide an image, the model runs it as-is.  Nothing here is
affiliated with or endorsed by any hardware vendor, and the project is not part
of or endorsed by the QEMU project.
