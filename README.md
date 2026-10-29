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

With GICv2 and `sheng-smp=1`, `armcpu-cycle-clock=1` (or `cc=1` when the
long option is absent) samples the PMU cycle
counter on each secondary during `initCPU`, before enabling interrupts. The
board recovery integration must also be compiled and its RTC/watchdog
recheck must pass; otherwise the sample is refused. The board's ordinary
watchdog feeder is stopped before any PMU access. If visible PMU interference
is detected, the saved PMU state is not restored, the gate remains held, and
the watchdog is expected to attempt recovery. This is not a guarantee against
other watchdog feeders or an upper-EL trap. Status 11 means interference,
12 means the board gate was unavailable or not compiled, and 13 means the
recovery recheck failed. No sample is valid in these cases.
The
completed sample and that core's MPIDR are published to an independent,
interrupts-enabled reporter thread. It reports `cycle-clock-start` with the
`processor_start` result and the startup thread's interrupt state before and
after the call. It waits at most 15 seconds (or 150 polling sleeps) for both results, then logs `cycle-clock-incomplete` once and exits. Published results remain in their mailboxes even if they arrive later; no late UART report is emitted. A startup-thread creation
failure is reported as result -1000. Each reporter log checks interrupt state; on a disabled state it suppresses the log, records reason 2 in `armcpu_cycle_reporter_failed`, and does not sleep; the standalone reporter restores interrupts before terminating. A reporter-thread creation failure is
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
flag already set; 4 timer timeout or read limit (262,144 total or 4,096 consecutive unchanged reads); 5 zero cycles; 6 elapsed time exceeds 2 ms;
7 cycle-counter overflow; 8 the sum of the two PMU read brackets
exceeds 1% of the target interval (rounded down to whole timer ticks); 9 a consecutive timer step moves backwards (a modular delta greater than half the 64-bit range); 10 neither virtual nor physical counter advanced during bounded preflight. Status 9 also reports zero ticks. `cycle-clock-time` reports the four raw selected-counter readings; `cycle-clock-bad-time` records the first backward pair and its stage (1 initial bracket, 2 polling, 3 final bracket). The report is split into short lines to reduce serial interleaving. Failed samples report zero cycles. The diagnostic
uses the 32-bit counter mode, rejects overflow, clears only its newly generated
cycle overflow flag, and restores the previous PMCR and counter. The 2 ms
bound is measured using CNTFRQ and the selected architectural counter over the outer timer readings enclosing both
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

The diagnostic probes CNTVCT before and after enabling the virtual timer and probes CNTPCT after enabling it only when CNTVCT did not pass, with at most 1,025 reads per probe. Probe state 0 means no observed progress, 1 progress, 2 backward movement, and 3 not probed (physical counter only). It selects CNTVCT when it advances, otherwise CNTPCT only if that advances; it reports the selected source and rejects either source if later measurement fails its bounds. The probe shows liveness, not that a counter has the advertised rate or that its use is safe on every platform. This remains an opt-in diagnostic, not a timer handoff fix. Enabling CNTV_CTL_EL0 controls the timer interrupt, not the system counter. Actual source access and rate require hardware verification.

`raw-cpuN` control lines each have exactly one variadic 64-bit value: a known format constant, a direct CNTVCT read, and, only when the physical probe is needed, a direct CNTPCT read. They use independent per-CPU single-word storage rather than the cycle sample struct; the flag and mailbox publication order the stores. Comparing the constant and raw values with the multi-value lines helps distinguish formatting and struct-copy problems, but is not proof of timer correctness. Interrupt checks still apply to every line.

### ARM64 instruction separators

Multi-instruction inline assembly uses newline separators, including the GICv2 redistributor wait counter, timer and PMU reads, virtual-timer enable, GIC barriers, GICv3 SGI and EOI paths, diagnostic register reads and interrupt masking, and cache-clean barriers. R4's recovered ArmCpu object contained consecutive ISBs with no counter MRS at the preflight reads; the emitted registers were therefore not established by those instructions. The changes restore the intended instructions, but their device effects remain unverified. For a compiled ArmCpu.o, `python3 tests/check-arm64-rea.py /absolute/path/ArmCpu.o` checks the actual initCPU object instructions with REA, including ISB immediately followed by each CNTVCT, CNTPCT and PMCCNTR MRS. It also checks the redistributor wait counter, virtual timer write and GIC signal barriers. This requires REA with its Ghidra provider.

`ARMCPU_SPI_SERIALIZE=1` serializes concurrent SPI registrations with local
interrupts masked. The default is off. This protects the registration table
and distributor register read/modify/write between callers of
`armcpu_spi_attach`; it does not configure a board SYSIRQ polarity controller
or establish ownership against unrelated register writers. The handler runs
in primary interrupt context and must clear or mask a level source.

With `ARMCPU_SPI_SERIALIZE=1`, `armcpu_spi_attach_sysirq` also serializes
MTK SYSIRQ polarity changes with GICv2 registration. It refuses a previously
owned or enabled SPI, validates group, priority, target, trigger and polarity,
and masks that SPI plus restores its previous configuration on failure. The
owned-only mask operation is safe from primary interrupt context. All SYSIRQ
writers must use the same controller management for serialization to apply.

`ARMCPU_GICV2_ASSUME_NONSECURE=1` is a board-specific build opt-in, off by
default. Enable it only when the distributor MMIO accesses are known to be
Non-secure. If GICD_TYPER.SecurityExtn is set, the checked SYSIRQ attach then
does not test or write IGROUPR, which is RAZ/WI to Non-secure accesses. It
still requires priority 0xa0, target, trigger, polarity and enable readback.
Group 0 priority fields are RAZ/WI to Non-secure accesses, so the nonzero
priority readback is evidence for Group 1 only under the stated access
assumption. The flag cannot detect a mistaken Secure mapping. When Security
Extensions are absent the existing IGROUPR check remains in force. Neither
readback nor registration demonstrates interrupt delivery; test actual
delivery and the device driver's timeout, quarantine and fallback paths.
In a successful checked attach, readback bit 16 marks Group 1 as inferred from
the Non-secure assumption and priority, rather than observed via IGROUPR; the
legacy lower bits remain unchanged. This marker applies only to success.
See Arm IHI0048B sections 3.5 and 4.3.4:
https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe

The checked registration rollback and the owned mask operation read the
ISENABLER bit after ICENABLER. An unconfirmed mask is recorded in
`armcpu_spi_mask_failures` and `armcpu_spi_mask_failed_id` and is not reported
as successful. If rollback cannot mask a source, the published handler and
its state remain alive. This lock coordinates only users of this API; an
unrelated writer that bypasses it can still race on the SYSIRQ polarity word.
