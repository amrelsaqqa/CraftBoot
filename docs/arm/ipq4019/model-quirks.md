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

## Open: the QPIC NAND install is randomly inconsistent

Status as of the current session: a netinstall reports `Software installed.`
and the bootloader then loads the kernel, but the installed system's data is
sometimes damaged, and the damage moves around (`yaffs: write required N
attempts`, `yaffs: **>> Block N needs retiring`, `yaffs: marking block 0 bad`,
`ERROR: could not mount filesystem`).  It is timing-dependent: the same run can
end with a perfect die and the next one with a die whose main partition has
data but no yaffs object headers.  Warm reboots sometimes come up, cold boots
of a damaged die never do.

### What is proven

- The die scan is the ground truth: a good install leaves the boot partition's
  `kernel` object plus a main partition holding the yaffs tree (`boot`, `rw`,
  `nova`, `etc`, `var`, `pdb`, `store`, ~170 file headers, ~37 dirs); a bad run
  leaves main-partition data pages with zero object headers.
- The program path is column-correct: on one page the driver issues four
  `op=6` EXECs with the page address written once (`addr0` identical, `col=0`)
  and the model programs `at=0/516/1032/1548` with `n=516/516/516/500`, which
  tiles the 2048-byte page exactly; the erase carries its row in
  `ADDR0[15:0]` and decodes to the right block.
- The store mirror is not the culprit: a traced run's die content matches what
  the model programmed (the good die above), so `qpic_die_persist` reaches the
  file.
- A real race was found and fixed (commit `af4a5f5`): the program payload
  arrives on the BAM data pipe while the EXEC arrives on the command pipe, and
  each doorbell drains only its own pipe.  `MIN(staged, codeword)` therefore
  programmed short pages.  2448 of 5014 programs hit that path in one traced
  install; the EXEC now waits for its payload (a pending queue, completed in
  order as data arrives).
- A column-stride experiment (deriving the codeword from `col/qpic_cw_size`
  instead of `col/528`) was tried and did **not** help; it is not in the tree.

### What is still suspected

Both remaining stream mechanisms are shared, order-dependent state:

- `tx_stage`/`tx_len` in `IPQQpic`: data pipe appends, command pipe consumes.
  Short consumption is prevented now, but the queue is still process-wide and
  any interleaving between pages leans on the two pipes draining in a
  favourable order.
- `rd_stream`/`rd_len` plus the `rx_addr/rx_len/rx_done/rx_n` descriptor ring:
  the command pipe appends per-codeword windows, the data pipe drains them into
  the guest's rx buffers.  The stream resets on `cw == 0 || page != rd_page`,
  which is a codeword counter, not a page event; a drain that lands between the
  reset decision and the next fill mixes pages.

The refactor under consideration makes both sides page-scoped and
order-independent: a `wr_page`/`wr_buf[NAND_STRIDE]` staging page where the
data pipe writes bytes at their column and a program EXEC commits a codeword
only when that codeword's bytes are present, and a `rd_page`/`rd_buf` with a
byte cursor for the read side that resets on a page change rather than on a
codeword index.  The die model (column-addressed program, block erase, store
persist) is believed correct and should stay.

Separately, one observation that may or may not be the same bug: cold-booting
a die whose yaffs tree is intact still ends at `ERROR: no system package
found!` about 2 s in, with no `yaffs` mount line in the log at all — the
installed system's init (the 1620K initramfs) decides the package is absent
before mounting anything.  A full 256 MB RAM dump at that panic is at
`/tmp/ram-panic.bin`, and the string sits at `0x809fdcfb`.  If the refactor does
not change this, it is a second, separate issue.

### Reproducing and testing

Build (see the model iteration note in the README — `sync` is seconds):

```sh
make TARGET=arm/ipq4019 sync
```

Run the board with a *copy* of a die and the console on a socket:

```sh
cp ~/nand-test-ac3.bin /tmp/die.bin
QEMU_IPQ_QPIC_LOG=1 QEMU_IPQ_QPIC_PGM=1 \
build/arm/qemu-system-arm \
  -M "ipq4019,board=hap-ac3,qpic-nand=on,eth-bridge-port=1,qpic-nand-file=/tmp/die.bin" \
  -kernel ~/ac3-nor-7.17.bin -display none -monitor none \
  -chardev socket,id=con,host=127.0.0.1,port=4444,server=on,wait=off,mux=on,logfile=/tmp/console.log \
  -serial chardev:con \
  -netdev tap,id=net0,ifname=ipqtap0,script=no,downscript=no
```

`telnet 127.0.0.1 4444` is the board console; netinstall needs character mode or
`^E` is eaten.  dnsmasq must be stopped (it owns the tap).  At the banner `^E`
(etherboot), or `g` in the router's setup menu, then run `netinstall-cli` with
the system and option packages.  The QPIC trace with `QEMU_IPQ_QPIC_LOG=1` to
stderr is the record to analyse; without it every conclusion is a guess.

Acceptance criteria before any refactor is committed:

1. five installs in a row (each starting from a fresh copy of the same die),
   each followed by a cold boot, all reaching the installed system;
2. five warm reboots (`/system reboot`) of one install, all coming back;
3. three cold boots of one good die, all identical;
4. in every case the console shows `0 blocks to be sorted`, no
   `needs retiring`, no `write required N attempts`, no `marking block bad`,
   and the die scan shows the yaffs tree in the main partition.

Deferred until all of the above: the debug hooks (`QEMU_IPQ_QPIC_PGM` and the
`pc/lr` in the QPIC log lines) can stay, they are only active when the env
variables are set.
