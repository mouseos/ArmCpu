# a04br3 runtime cycle experiment

The generic ArmCpu build defaults `ARMCPU_RUNTIME_A04BR3` to 0. The a04br3
device build may set it only after checking the generated XNU configuration
has `CONFIG_CPU_COUNTERS=0`. The device default does not compile this experiment. Even when compiled, it is off unless the boot
argument `armcpu-runtime-cycle=1` (or the short form `arc=1`) is supplied and the ADT root compatible
starts with `benesse,a04br3`. No runtime worker is created otherwise.

The worker waits five seconds and then up to 20 seconds for the board's
recovery-ready flag. It then attempts CPUs 0–3 in three rounds, one second
apart. Recovery-ready requires a verified RTC fastboot bit and watchdog enable
readback. The worker stops the board's periodic watchdog feed before binding
and until the probe returns. If the worker or an upper EL does not return, the
31-second watchdog is intended to reset into fastboot. This relies on no other
agent feeding or disabling that watchdog and has not been validated on target.

Each attempt nonblockingly acquires XNU's `processor_updown_lock`, which XNU
processor startup and shutdown also use. It rejects an observed quiesce/halt,
an incomplete secondary startup, an offline processor, or a busy lock. It binds
the current thread and yields; after resuming it rejects a delay of 250 ms or
more, checks the actual processor, masks local interrupts and disables
preemption. It probes CNTVCT, checks PMU registers are unused, saves state,
measures PMCCNTR for a busy-loop target of 0.9 ms, and restores state. Outer
windows over 1 ms are rejected. The lock, binding, interrupts and preemption are
restored before output. `thread_block()` cannot enforce the 250 ms deadline
until the worker is scheduled again; the watchdog is the recovery fallback,
not proof of a hard scheduler deadline.

Every sample reports the raw CNTFRQ, ticks, cycles, outer and endpoint widths,
PMCR, PMC enable, filter, interrupts and overflow flags. `busy=1` and
`idle=unknown` mean this is an intrusive active-load sample, not a workload or
idle average. `owner=assumed-kpc-disabled` is deliberately an assumption, not
a global lease. The generated CONFIG_CPU_COUNTERS=0 disables the normal XNU
KPC/monotonic integration; R5 showed secondary CPUs 1–3 completing the startup
PMU probe. Neither fact proves that EL2, EL3, firmware or debug agents never
access the PMU. CPU0 PMU access has not been directly validated. The explicit
boot flag and recovery path authorize an experiment, not proof that traps or
conflicting external ownership are impossible.

The host tests model CPU binding, lock refusal, failed recovery, timeout, PMU
busy refusal, restoration and IRQ-safe logging. They cannot prove hardware
access, scheduling deadlines, watchdog recovery or external PMU ownership.

The recovery-ready bit is a one-time observation, not a fresh guarantee for
each sample. The outer-window limit rejects late results after the fact; it
does not impose a hard upper bound on the IRQ-disabled interval. No PMU check
provides an exclusive lease against EL2, EL3, firmware, or debug agents.
Do not treat a successful sample as proof of ownership or guaranteed recovery.
