# ArmCpu

ARM64 CPU, GIC and timer driver for UniXNU. Publishes the CPU platform device, the interrupt controller and the architected timer.

Current state: starts the CPU, GIC and timer on sheng and on the QEMU virt machine.

Linked into XNU by `unixnu build kernel`.

## GICv2 (optional)

Boards with a GICv2 interrupt controller (GIC-400, for example the MediaTek MT8167 of the Benesse TAB-A04-BR3) build
the driver with `CONFIG_XNU_ARMCPU_GICV2=y` in their defconfig, which compiles ArmCpu with `ARMCPU_GICV2=1`. The GICv2
path is then selected by `/arm-io/intc` `compatible = "arm,gic-400"` or `"arm,cortex-a15-gic"`, whose `reg` holds the
distributor and the CPU interface. Without the flag the GICv2 code is not compiled and the GICv3 driver is unchanged.

The GICv2 register use (non-secure group, GICC_IAR/GICC_EOIR, GICD_SGIR with the banked GICD_ITARGETSR0 target bit,
GICD_ITARGETSR routing for SPIs) follows the MT8167 bring-up in
[xnu_kernel_benesse_a04br3](https://github.com/mouseos/xnu_kernel_benesse_a04br3) (pexpert/arm/pe_a04br3_gic.c), which
ran on that tablet.

On GICv2 the architected virtual timer is PPI 27 and IPIs are SGI 1; `reg` may also list the GICH and GICV regions,
and the distributor must identify itself as GICv2 (GICD_PIDR2 ArchRev 2). `armcpu-spi-ipi` has no effect, because it
depends on GICv3 affinity routing. Every core sends itself one SGI from `initCPU`, so that XNU clears its
`SIGPdisabled`; `armcpu-self-ipi=0` turns this off.

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

### Opt-in secondary cycle-clock diagnostic

With GICv2 and `sheng-smp=1`, `armcpu-cycle-clock=1` samples the PMU cycle
counter on each secondary during `initCPU`, before enabling interrupts. The
completed sample and that core's MPIDR are published to an independent,
interrupts-enabled reporter thread. It reports `cycle-clock-start` with the
`processor_start` result and the startup thread's interrupt state before and
after the call. It emits `cycle-clock-pending` initially and every five seconds
while either the start result or sample remains unavailable, and continues to
wait for a late sample even after a start failure. A startup-thread creation
failure is reported as result -1000. A reporter-thread creation failure is
recorded in `armcpu_cycle_reporter_failed` and logged if interrupts are enabled.
Neither incomplete samples nor unreturned start calls are treated as success.
The
UART line `ArmCpu: cycle-clock` reports CPU, status, CNTFRQ, timer ticks,
cycle delta, actual MPIDR, original PMCR, cycle filter, interrupt enables,
overflow flags before and after measurement, the enclosing timer interval,
and the timer intervals around each PMU read. For status 0 the mean
active-cycle frequency in Hz is `cycles * cntfrq / ticks`; it is a roughly
1 ms busy-loop observation, not a guarantee of the clock under idle or a
later workload. Status 1 means no advertised PMU, 2 invalid timer frequency,
3 PMU already enabled, cycle filtering active, or any interrupt or overflow
flag already set; 4 timer timeout; 5 zero cycles; 6 elapsed time exceeds 2 ms;
7 cycle-counter overflow; and 8 the sum of the two PMU read brackets
exceeds 1% of the target interval (rounded down to whole timer ticks); 9 a consecutive timer step moves backwards (a modular delta greater than half the 64-bit range). Status 9 also reports zero ticks. `cycle-clock-time` reports the four raw CNTVCT readings for diagnosis. Failed samples report zero cycles. The diagnostic
uses the 32-bit counter mode, rejects overflow, clears only its newly generated
cycle overflow flag, and restores the previous PMCR and counter. The 2 ms
bound is measured using CNTFRQ and CNTVCT over the outer timer readings enclosing both
PMU reads, including delays during either read. The reported ticks use the
midpoints of the two read brackets as an estimate of the sampling interval.
Successful samples require the sum of those bracket widths to be no more than
1% of the target; this bounds endpoint timing uncertainty, not all sources of
frequency measurement error.
The core MPIDR is checked before any PMU access. The boot CPU is deliberately
excluded. EL2 and EL3 PMU trap and security restrictions are not established;
EL1 PMU access on secondaries still requires hardware validation; a trap can prevent startup before a line is
printed. Use the existing watchdog and fastboot recovery path for an experiment.
The diagnostic is absent unless the boot argument is explicitly set to 1.
