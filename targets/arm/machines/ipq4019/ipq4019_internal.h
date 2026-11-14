/*
 * Qualcomm IPQ4018/IPQ4019 machine -- internal interface between blocks.
 *
 * The machine is one SoC: the same die, the same controllers, the same
 * addresses.  Everything outside it is a board question and lives as data in
 * ipq4019_board.c.  The C is split by controller, one file per address block,
 * because that is where the volume is and where a reader needs a boundary:
 *
 *   ipq4019.c          machine init, memory map, reset, -M properties
 *   ipq4019_board.c    board presets, config merge, DTB ports
 *   ipq4019_dump.c     whole-machine state dump and diagnostics
 *   ipq4019_stub.c     shadowed stubs, register-file block, flash window, gcnt
 *   ipq4019_flash.c    BLSP1 QUP SPI, NOR die, SPI NAND, die stores
 *   ipq4019_gpio.c     TLMM, LEDs, keys
 *   ipq4019_uart.c     BLSP UARTDM
 *   ipq4019_eth.c      IPQESS, host net backend, MDIO/PHY
 *   ipq4019_bam.c      BLSP1 BAM (QUP side)
 *   ipq4019_qpic.c     parallel NAND die and QPIC controller
 *   ipq4019_qpic_bam.c QPIC BAM pipes and command elements
 *
 * A block owns its register offsets, its MemoryRegionOps, its state and one
 * ipq4019_<block>_init(IPQ4019State *).  Anything another file needs is
 * declared here; nothing else crosses a file boundary, so this header is the
 * whole interface and it should stay small.
 */
#ifndef HW_ARM_IPQ4019_INTERNAL_H
#define HW_ARM_IPQ4019_INTERNAL_H

#endif /* HW_ARM_IPQ4019_INTERNAL_H */
