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
