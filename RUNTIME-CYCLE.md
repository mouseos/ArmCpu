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

The active-interval calculator itself is not connected to the runtime worker
and performs no PMU accesses. Its bounds cover only
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
calibration remain unverified. The optional `ARMCPU_RUNTIME_WINDOW_A04BR3` worker binds each CPU, keeps the
watchdog gate and local lease during a 2 ms scheduler interval, and rejects a
window if the XNU idle epoch changed. It requires `arw=1` or the long boot
argument, a04br3 compatible, GICv2 and `XNU_ARMCPU_IDLE_EPOCH=1`. The board
provider currently refuses every acquisition because upper-EL/debug ownership
and access are unqualified: this build cannot produce measurements. Host mocks
exercise the code but do not establish the missing platform contract. A WDT
readback or past R8 result is not a PMU lease. The worker includes IRQ time and
rejects intervals with any machine-idle transition. If recovery or release is
not confirmed, it retains the gate and permanently quarantines further window
probes for the boot; successful recovery is not guaranteed.


### Repeated experimental active windows

`arw=1` retains the single pass; `arw=2` explicitly requests 120 passes,
with a 5 second sleep between passes and a 10 ms scheduled observation per CPU.
The existing 50 ms deadline and quarantine rules still apply. Each pass releases
the PMU lease, watchdog gate and hotplug lock before sleeping. A poisoned window
stops subsequent passes. `runtime-attempt` identifies round and CPU, and a
successful `runtime-time` reports CNTVCT bounds and CNTFRQ for the following
`runtime-window` record. The CPUs are sampled sequentially, not simultaneously;
these are active-interval averages, excluding machine idle and idle IRQ handling.
The timer remains uncalibrated and higher EL/debug PMU ownership unverified.
This is an experimental opt-in, not a hardware watchdog recovery guarantee.

`arw=3` is a separate opt-in context mode requiring the active backend. It
performs the same bounded repeated windows and invokes an optional board hook
before acquiring the PMU lease and after successful restoration and release.
The hook reports board clock and load context independently; its reads and logs
are not an atomic snapshot and instrumentation itself changes scheduling and
load. Missing hooks and unavailable board data are recorded as such. The
bracketing reads do not prove a register remained constant during a window.


### CPU3 preemptible busy control (experimental, hardware gated)

`XNU_ARMCPU_BUSY_CONTROL=1` passes an opt-in compile flag through the device
builder when GICv2, the runtime window, idle epoch, active backend and XNU switch observer are
selected. `arw=4` currently refuses on hardware before any board hook, CPU
binding, gate or PMU access. The candidate workload is compiled when enabled
but runs only in host simulation until the actual kernel integration, meter
exclusion and independent review are complete. The default is unchanged.

The API is weakly imported and refuses if it is absent or cannot return a
snapshot; IOKit does not access the private XNU thread structure.

The candidate keeps the existing short preemption-disabled PMU setup and
restore sequences. Between them the bound worker runs a short timer-read loop
with preemption and IRQ enabled. It snapshots the worker's XNU `c_switch` count
before enabling preemption and after disabling it again. A changed counter,
wrong CPU, idle epoch change or multiple active intervals invalidates the busy
sample; lease loss and PMU interference retain existing quarantine behavior.
A CPU bind followed by `thread_block` is required before PMU access. The target
XNU's `sched_prim.c` increments `c_switch` on dispatch of a different thread;
this is not evidence that IRQs did not run. IRQ time and instructions are
included, and equal 32-bit counters cannot rule out an exact wrap. The timer
loop has iteration and stall limits and rejects overly long completed windows;
these do not guarantee a wall-clock upper bound for a descheduled worker.
Unlike a non-preemptible busy loop, waiting to be rescheduled does not extend a
new preemption-disabled section. PMU lease, watchdog gate and hotplug lock are
still retained until the worker can restore state; a non-return still relies
on existing watchdog recovery and cannot be claimed safe by host tests.

The host simulation compares CPU3 normal and busy rounds with the same boot,
separate leases, near PLL/FHCTL snapshots and explicit pair validity records.
Skip, failure, missing near snapshots, a switched-out worker or an idle change
invalidates the pair; a missing pair record is unknown. Context and near hooks
do not invoke a frequency meter, but this does not exclude an independent
meter. Board register readings cover endpoints only. Timer calibration and
higher-EL/debug PMU ownership remain unverified. Host mocks do not prove the
real thread ABI, code generation, scheduler, watchdog, meter exclusion, or IRQ
behavior, so must not be treated as authorization for hardware use.
