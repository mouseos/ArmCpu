# a04br3 runtime cycle experiment

The production policy currently refuses every CPU, even when explicitly
compiled and requested. It does so before binding, stopping watchdog feeds or
accessing PMU registers. Enabling measurement requires a separately reviewed
system-wide ownership contract, higher-EL access evidence, and validated
recovery; neither a boot flag nor CONFIG_CPU_COUNTERS=0 establishes these.
The worker description below documents the experimental implementation, not
an assurance that it is safe to enable on hardware.

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
more, checks the actual processor, keeps local interrupts enabled and disables
preemption. It probes CNTVCT, checks PMU registers are unused, saves state,
measures PMCCNTR for a busy-loop target of 0.9 ms, and restores state. Outer
windows over 1 ms are rejected. The lock, binding, interrupts and preemption are
restored before output. `thread_block()` cannot enforce the 250 ms deadline
until the worker is scheduled again; the watchdog is the recovery fallback,
not proof of a hard scheduler deadline.

Every sample reports the raw CNTFRQ, ticks, cycles, outer and endpoint widths,
PMCR, PMC enable, filter, interrupts and overflow flags. `busy=1` and
`idle=unknown` mean this is an intrusive active-load sample, not a workload or
idle average. `owner=unverified-kpc-disabled` explicitly records that no global lease
was obtained. A detected PMU configuration change yields status 11, stops
further sampling, leaves the observed state untouched rather than restoring
a stale snapshot over another actor, and keeps the board watchdog unfed.
Watchdog recovery is an intended fallback, not a guarantee. Undetected changes
remain possible. The generated CONFIG_CPU_COUNTERS=0 disables the normal XNU
KPC/monotonic integration; R5 showed secondary CPUs 1–3 completing the startup
PMU probe. R8 also produced samples on CPU0, but from a different kernel and
with assumed ownership. Neither observation proves that EL2, EL3, firmware or
debug agents never access the PMU in the current build. The explicit
boot flag and recovery path authorize an experiment, not proof that traps or
conflicting external ownership are impossible.

The host tests model CPU binding, lock refusal, failed recovery, timeout, PMU
busy refusal, restoration and IRQ-safe logging. They cannot prove hardware
access, scheduling deadlines, watchdog recovery or external PMU ownership.

The board's nonblocking watchdog gate serializes its own normal feeder with
the transition into probing. A busy gate rejects the sample. The recovery
recheck reads both MODE and LENGTH as well as the RTC fastboot bit. This does
not prevent other agents or explicit reset paths from feeding, disabling, or
resetting the watchdog, nor prove that a trapped CPU will recover.

The worker rechecks the RTC fastboot bit and watchdog enable after binding;
known board WACS2 users share a nonblocking lock. These are observations, not
leases over firmware or hardware. Local IRQs remain enabled during the sample,
which includes interrupt work; the outer-window limit rejects late results
after the fact. No PMU check provides an exclusive lease against EL2, EL3,
firmware, or debug agents.
Do not treat a successful sample as proof of ownership or guaranteed recovery.

For a successful interval, the worker additionally reports conservative
integer frequency bounds derived from the timer reads immediately surrounding
each PMCCNTR read. The lower bound is rounded down and the upper bound up.
These bounds cover endpoint timing uncertainty only: they do not calibrate
CNTFRQ, establish counter semantics, compensate for interrupts or identify
an idle or instantaneous operating frequency. Invalid intervals produce no
bounds. The currently unconditional refusal still prevents the worker from
entering the experimental sampling code.

`tools/audit-pmu-macho.py` inventories selected architectural PMU and MDCR
instructions in executable segments of a supplied arm64 Mach-O (requires the
Python capstone package). Its report is static evidence, not proof of reachability,
exclusive ownership, absence of dynamically generated code or higher-EL safety.

## Active-interval calculation (not yet connected)

`RuntimeActiveClock.h` computes a sum over explicitly identified active
intervals and returns conservative bounds using timer brackets around each
cycle read. It rejects missing lease or boundary evidence, migration, lease
or configuration changes, ambiguous counter wrap, overflow, backwards time,
invalid endpoint brackets and arithmetic overflow. It performs no hardware
access. A boolean or generation supplied by an untrusted caller does not grant
or prove a PMU lease; the producer must establish one, maintain it across all
intervals and prevent another actor from changing and restoring state between
snapshots. `activeBoundaryKnown` requires actual per-CPU execution/idle
boundary hooks, not a boot option or a scheduler-utilization estimate.

No production producer is connected: this calculator does not enable the
runtime worker, touch the PMU, or make CPU0 measurable. Its bounds cover only
endpoint timing in nominal CNTFRQ units, and do not prove PMCCNTR semantics
across WFI, secure state, EL transitions, interrupts, or timer calibration.

For a valid active-interval result, producers must prevent counter reset and
any clearing of the cycle overflow flag throughout the lease, or independently
prove every measured interval is shorter than the 32-bit wrap time. They must
also guarantee the timer advances monotonically modulo its verified width and
that each observed elapsed interval is less than half its range. The current
calculator treats the timer as a 64-bit counter; narrower implementations
require a different, verified extension. IRQ time occurring between active
boundaries is included, even though PMU interrupt-enable state must be zero.
The active producer must define its treatment of WFI, EL filtering and secure
execution before describing the resulting bounds as active CPU frequency.

`RuntimeClockWindow.h` is a non-busy two-snapshot transaction prototype: it
saves and enables the local cycle counter, permits other work between entry and
exit, refuses any intervening idle epoch, then restores the original state. Its
caller must provide a real exclusive, non-revocable per-CPU PMU lease and keep
preemption disabled during each register sequence. It also must hold the board
watchdog gate before the first access, release it only after positive evidence
of complete restoration, and quarantine the boot on conflict or non-return.
The `valid` and `watchdogHeld` methods are contract checks, not an ownership
implementation. Endpoint comparisons cannot detect transient changes, and
access traps, PMU counter behavior, reset, WDT recovery and elapsed timer
calibration remain unverified. No target producer or lease provider is wired
up; this prototype does not make runtime measurement available.
