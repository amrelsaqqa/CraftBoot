# ipq4019 — model quirks and deliberate deviations

Everything here is a choice, not an accident: either the SoC has a block the
machine does not model, or modelling it exactly would make the emulator depend
on guests instead of hardware.

## The boot chain and the QSEE handover

The boot flash holds a chain of ELF images: the first stage at offset 0, the
trusted-execution image it loads next, and the application bootloader that runs
when the trusted side returns.  The machine reads the three entry points out of
the supplied image:

```
ipq4019: stages: sbl1@0x0a8c1490 qsee@0x87e80000 appsbl@0x80000000
```

The trusted-execution image is **not executed**.  Its early bring-up performs
an illegal access and expects the protection fabric (NoC/XPU/VMIDMT) to trap
it; without that fabric modelled the guest panics.  The policy is *trusted side
returned OK*: when the CPU reaches that image's entry point the machine sets
`PC` to the application bootloader's entry, `r0 = 0`, and clears Thumb (the
bootloader is ARM code while the first stage calls it from Thumb state).

The handover is caught with a QEMU breakpoint carrying a machine-only flag
(`BP_MACHINE`) and a small hook: when the exec loop matches the address, the
hook changes the CPU state and returns true, and execution continues from the
new PC.  Nothing in the guest is modified, no host process stays attached, and
SBL1 reloading the image on the next boot does not disturb it.  The redirect
also survives a system reset.

```
ipq4019: qsee: skip qsee@0x87e80000 -> appsbl@0x80000000 (lr=0x0a8d191d, skip #1)
```

`QEMU_IPQ_QSEE=run` leaves the trusted image to execute instead (its first step
would be modelling the XPU).

## Single CPU

`smp` is one core.  The warm-boot register for the secondaries lives behind the
secure side, so Linux prints `Failed to set CPU boot address, disabling SMP`
and carries on.  The GIC is built with four CPU interfaces because the board
device tree lists four; a single-interface GIC gives the guest empty SGI/PPI
target masks and its IPIs are then silently dropped, which shows up much later
as a hang on `reboot`.

## Absent hardware reads as absent

- PCIe windows are decode-only: the guest sees an abort instead of a zeroed
  or all-ones register, so probes fail cleanly.
- The crypto engine window answers with `~0` (an unknown revision).  Faking a
  known revision only moves the failure deeper, into rings and BAM state.
- The WLAN register windows answer, but their stubs return `0` rather than the
  default `~0`: every "radio ready" poll would otherwise succeed and the driver
  would continue with state that was never initialised.  `QEMU_IPQ_WIFI_READ`
  overrides the value.
- USB and PCIe controllers are stubs; the boot path does not need them.

## Power-off

There is no power-off register on this SoC.  Software that stops the machine
prints `Power down`, masks IRQs and spins in `cpu_do_idle()`; on real hardware
the CPU parks with an interrupt pending that cannot be delivered.  A live guest
never has that fingerprint, so the machine recognises it (masked IRQs, pending
interrupt, not halted, held for 1.5 s) and ends the process the way removing
power would.  `poweroff=off` keeps it running for inspection, and
`QEMU_IPQ_HANG_LOG=1` prints the park state.

## Fault and reset visibility

A synchronous external abort is always a modelling gap, so it is logged
unconditionally:

```
arm: external abort at phys 0xa04f014 (vaddr 0x9104f014, 4 byte write, pc 0x7f64c6f8)
```

`QEMU_IPQ_FAULT_LOG=1` logs every ARM fault with `pc`, `lr` and `sp`, and
`QEMU_IPQ_RESET_DUMP=<file>` writes DRAM out as the machine resets, which is
where a guest's own panic text still lives when its console gives no output.

## What is still missing

- The trusted-execution image is skipped, not run (see above).
- The switch forwards a single front-panel port to a host netdev; traffic
  between the other ports is not forwarded.
- There is no WLAN radio behind the register windows: drivers give up cleanly
  instead of panicking, and wireless never comes up.
- The QPIC NAND is fully modelled, but a guest whose own driver does not
  understand the vendor's in-band ECC layout will not mount the partition; the
  layout is documented in `boot-and-storage.md`.

## The QPIC NAND install path

A netinstall reports `Software installed.` and the installed system boots,
warm and cold.  Getting there exposed two independent problems; both are
recorded here because they look alike from the console.

### Program window sizing (fixed)

An ECC page is four codewords, and the driver's buffer tiles the 2048-byte
page as 516, 516, 516, 500.  The model used to size the last codeword from
the physical codeword (500 data + 16 spare + 7 parity + 4 CFG bits = 527),
which the driver never sends.  Before the held-EXEC queue that was masked by
`MIN(staged, need)`; once the EXEC waited for its whole payload the last
window never completed, the *next* page's first 27 bytes completed it, and
every page after the first was written 27 bytes off -- with the last window
running past the page into the spare area.  yaffs then reported `write
required N attempts`, `Block N needs retiring`, `marking block 0 bad` and
`could not mount filesystem`, and the die scan showed main-partition data
with no object headers.

`qpic_program_size()` now derives the window from the page tiling
(`NAND_PAGE - (steps - 1) * cw_size`, i.e. 500 for the traced configuration)
and `qpic_program_page_bytes()` is the invariant it is checked against.

### Page-scoped program staging

The data pipe and the command pipe are rung independently, so either can
arrive first.  The model now tags every staged window with its page and
checks the bytes moved per page; a window of a different page arriving while
one is still waiting drops the staging (loudly) instead of shifting it into
the next page.  Column-addressed (spare/bad-block) programs move their own
window, bounded by the row end, rather than a whole codeword.  Read
transactions are either one page or one column window, never a stream that
silently carries stale bytes across a page, and no staging/read/rx-ring
overflow can drop data silently.

All of these print `qpic: anomaly: ...` (unconditionally, rate-limited) and
must stay silent on a correct run.

### Proven facts from the traces

- One install moves exactly 2048 bytes per programmed page: the data pipe
  delivers 512+4+512+4+512+4+500, the four EXECs consume 516/516/516/500,
  and every page starts with `in=2048` (7,953 pages in one traced install).
- The driver writes the page address once per page (`addr0` identical across
  the four EXECs, `col=0`), so a per-page step counter is correct.
- Read windows are 516/516/516/500 as well; both yaffs mounts (`mtdblock1`
  then `mtdblock0`) complete on the installed die.
- The die scan is the oracle: a good install leaves the boot partition's
  kernel object plus the main tree (`bin`, `boot`, `rw`, `nova`, `etc`,
  `var`, `pdb`, `store`, the `system`/`option` images; ~170 objects).
- The store mirror is not the culprit, and the column-stride experiment
  (`col/qpic_cw_size` instead of `col/528`) did not help; neither is in the
  tree.
- A real race was found and fixed earlier (`af4a5f5`): the program payload
  arrives on the BAM data pipe while the EXEC arrives on the command pipe,
  and each doorbell drains only its own pipe.  `MIN(staged, codeword)`
  programmed short pages (2448 of 5014 programs in one traced install); the
  EXEC now waits for its payload.

### The boot failure was a partition-offset mismatch

The console the init prints (`opendir: No such file or directory` twice, then
`ERROR: no system package found!`) is not a NAND failure.  The installed
kernel's command line carries `boot_part_size=8388608`, so its table is
`Boot 0x0-0x800000`, `Main 0x800000-0x8000000` -- the same values a real
RouterOS 7.x device reports.  A netinstall image from a matching build writes
the rootfs at 8 MB, the kernel mounts `mtdblock1` and `mtdblock0`, and the
system boots, warm and cold.

An older netinstall image defaults to a 6 MB boot partition, writes the tree
at 6 MB, and the installed kernel then finds the parent directories on one
side of the 8 MB boundary and their children on the other: the yaffs scan
succeeds, the package is unreachable, and init panics.  Using a netinstall
image from the same generation as the installed system fixes it; the model
needed no change.

### Reproducing

Build (see the model iteration note in the README -- `sync` is seconds):

```sh
make TARGET=arm/ipq4019 sync
```

Run the board with a *copy* of a die and the console on a socket:

```sh
cp ~/nand-test-ac3.bin /tmp/die.bin
QEMU_IPQ_QPIC_LOG=1 \
build/arm/qemu-system-arm \
  -M "ipq4019,board=hap-ac3,qpic-nand=on,eth-bridge-port=1,qpic-nand-file=/tmp/die.bin" \
  -kernel ~/ac3-nor-7.17.bin -display none -monitor none \
  -chardev socket,id=con,host=127.0.0.1,port=4444,server=on,wait=off,mux=on,logfile=/tmp/console.log \
  -serial chardev:con \
  -netdev tap,id=net0,ifname=ipqtap0,script=no,downscript=no
```

`telnet 127.0.0.1 4444` is the board console; netinstall needs character mode
or `^E` is eaten.  dnsmasq must be stopped (it owns the tap).  At the banner
`^E` (etherboot), or `g` in the router's setup menu, then run
`netinstall-cli` with the system and option packages.

Acceptance: no `qpic: anomaly:` lines; the console shows `0 blocks to be
sorted`, no `needs retiring`, no `write required N attempts`, no `marking
block bad`; the die scan shows the tree; warm and cold boots both reach the
system.
