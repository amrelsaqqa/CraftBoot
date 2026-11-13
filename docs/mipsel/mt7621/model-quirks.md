# Model quirks — MT7621

Things where the model knowingly differs from the SoC, and why.

## 1. Reported data-cache line size is 2, not 4 (`board=lg-gapm7200`)

`Config1.Dl = 4` (32-byte lines) is the hardware value and what the MIPS
kernel expects.  The parallel-NAND bootloader for this board, however, has a
bug in its DMA allocator:

```
n = size - 1 + 2 * line_size
p = kmalloc(n)
aligned = (p + line_size - 1) & ~(line_size - 1)
memset(aligned, 0, n)          /* n bytes from the *aligned* pointer */
```

The `memset` starts at `aligned`, not at `p`, so it writes
`round_up(p, line) - p` bytes past the allocation.  Its next free-chunk header
sits right there, so the following allocation fails and the bootloader prints
"unable to allocate fifos".  With `line <= 8` the pointer is already aligned
and nothing is overwritten, which is why a reported line size of 8 boots while
16/32/64 do not.

The model therefore reports `Dl = 2` for this board only; the true value is
used everywhere else.  `MT7621_DL` overrides it for experiments.  This is a
guest bug, not a cache-coherency gap: no `cache` instruction executes, the
line size is only a number to the bootloader.

## 2. DSA receive path: `fport` encoding and the delayed RX interrupt

The frame engine fills RX descriptor word 3 with the receiving port, and the
two software stacks that run here read it differently:

| stack | field | value |
|---|---|---|
| Linux `mtk_eth_soc` | `RX_DMA_FPORT_SHIFT` 19, `mac = fport - 1` | `1 << 19` (MAC0) |
| other (`raeth` style) | `ext(rxd4, 18, 3)` | `5 << 18` (CPU port) |

Using one encoding for the other stack is not "the wrong port": Linux computes
`mac = -1` and releases the descriptor before any counter moves, which looks
exactly like a silent backend.  The model picks the encoding from `qtx_en`
(only the Linux driver enables the QDMA transmit engine); `MT7621_RX_FPORT`
forces the port number.

Drivers also move the RX rings to *delayed* interrupts after PHY bring-up
(`PDMA_RX_DLY_INT_CFG`, `MTK_PDMA_INT_MASK = MTK_RX_DONE_DLY`, bit 30).  The
model raises the delayed bit as well as `RX_DONE_INT0`, otherwise NAPI never
polls and filled descriptors are never consumed.

## 3. Guest time: CPU clock, GIC counter width, watchdog

Two independent things made guest time wrong (`sched_clock` at 25 MHz,
timestamps jumping, minute-long NAND scans, RCU stalls):

- **CPU clock.**  `clk-mt7621` computes the CPU rate from `MEMC[0x648]`
  (`FBDIV` bits 10:4, `PREDIV` 13:12).  The model programs FBDIV 69 /
  PREDIV 1 → 875 MHz, matching the counter.
- **GIC counter width.**  The SoC's GIC has a 64-bit shared counter, and Linux
  registers sched_clock with 64 bits regardless of the advertised width.  The
  model advertises `COUNTERBITS = 8`, implements `COUNTERHI`, and treats the
  per-VP compare as a full 64-bit deadline.  Compare semantics follow the
  width: a 32-bit counter wraps and re-arms after each expiry; a 64-bit
  counter has no next lap, so a deadline at or behind the counter is
  considered delivered and the timer disarms until the guest writes a new
  compare.

The counter width is a device property (`counter-bits`, default 32-bit); the
MT7621 machine sets 64.  `MT7621_GIC_COUNTER_BITS` and `MT7621_CPU_PLL` exist
only for experiments.

The guest's own watchdog pets its timer for a while and then stops while
loading modules; emulation is slow enough to exceed the guest's timeout, so
the model warns instead of resetting.  `MT7621_WDT_RESET=1` restores the
reset.

## 4. NAND command 0xEC

The 5.15 NAND driver issues `0xEC` (cache read) during its bad-block scan and
polls `NFI_PIO_DIRDY`.  It is handled like `0x30`; without it every miss burned
the driver's PIO timeout and produced RCU stalls.
