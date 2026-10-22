# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## GICv2 (optional)

Boards with a GICv2 interrupt controller (GIC-400) compile ArmCpu with `ARMCPU_GICV2=1`: the board's archive builder
passes `-DARMCPU_GICV2=1`. The GICv2
path is then selected by `/arm-io/intc` `compatible = "arm,gic-400"` or `"arm,cortex-a15-gic"`, whose `reg` holds the
distributor and the CPU interface. Without the flag the GICv2 code is not compiled and the GICv3 driver is unchanged.

The GICv2 register use (non-secure group, GICC_IAR/GICC_EOIR, GICD_SGIR with the banked GICD_ITARGETSR0 target bit,
GICD_ITARGETSR routing for SPIs) follows the GICv2 architecture specification and an earlier bring-up on a GIC-400 board
(Credits).

On GICv2 the architected virtual timer is PPI 27 and IPIs are SGI 1; `reg` may also list the GICH and GICV regions,
and the distributor must identify itself as GICv2 (GICD_PIDR2 ArchRev 2). `armcpu-spi-ipi` has no effect, because it
depends on GICv3 affinity routing. Every core sends itself one SGI from `initCPU`, so that XNU clears its
`SIGPdisabled`; `armcpu-self-ipi=0` turns this off. An SGI with INTID 0 is handled like the IPI SGI; nothing sends it.

Secondary cores stay off by default, as on GICv3, and `sheng-smp=1` starts them through PSCI `CPU_ON`, using the
conduit from `/psci` `method`. On GICv2 boards the PSCI entry point is a small trampoline. It drops from EL2 to EL1
when the Trusted Firmware starts the core at EL2, then enters the kernel reset vector. TCR (`sysreg_restore`) and the
kernel TTBR (`cpu_ttep`) are written back to memory first. `sheng-smp-gic` is ignored, since the banked GICv2 SGI/PPI
registers can only be programmed by the core that owns them.

### Credits

- GICv2 bring-up, timer PPI 27, SGI 1, priorities and GICD/GICC ranges for MT8167: mouseos/xnu_kernel_benesse_a04br3,
  commits 81fea67 (boot Cortex-A35 registration and GICv2 timer/self IPI), 133e9ec (MMIO barriers) and fee5652 (PSCI
  secondary start, EL2-to-EL1 entry and GICv2 SMP IPIs, verified on all four MT8167 cores). The GICD/GICC ranges also
  match the stock A04BR3 device tree and the [mouseos/edk2-mtk](https://github.com/mouseos/edk2-mtk) A04BR3 port.
- The secondary-core cache write-back set before `CPU_ON` follows the design of dipa-studio/ios-sweet patch 0065, as
  adapted in fee5652.
- Register values come from the Arm GICv2 architecture specification, the GIC-400 TRM (GICD_PIDR2) and the Armv8-A
  Architecture Reference Manual (RES1 bits of CPTR_EL2 and SCTLR_EL1). `MDCR_EL2.HPMN = PMCR_EL0.N` follows the Linux
  arm64 EL2 initialisation.
