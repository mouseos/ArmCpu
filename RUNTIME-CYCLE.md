# Runtime cycle diagnostics

The default-off `armcpu-runtime-cycle=1` starts one worker on the boot CPU.
After five seconds it attempts CPUs 0–3 in three rounds with a one-second
pause between rounds. It checks that a processor is online, binds the worker,
blocks for the binding to take effect, checks the current processor, disables
preemption and local interrupts, and runs the R5 busy sampler on CNTVCT and
PMCCNTR. It checks the processor again and restores interrupts, preemption and
the previous binding before logging. The elapsed CNTVCT ticks and the full
outer window are reported, along with the individual PMU-read bracket widths.
The busy-loop target is 0.9 ms, reserving the remaining 0.1 ms for read
brackets; any reported outer window longer than 1 ms is rejected. A timer that fails the bounded preflight is not
used and the PMU is not changed. This observer intentionally adds active load
and does not measure idle time or the CPU's workload-wide average.

The sampler refuses a PMU that has any enabled counter, PMCR enabled, active
cycle filters, interrupt enables, or overflow flags. On a suitable idle register
state it saves the cycle count and PMCR, clears and enables only the cycle
counter, then disables it and restores both values. Each line identifies the
CPU; reports include raw PMCR, PMC enable, PMCCFILTR, interrupt and overflow
values, the counter and the timer frequency. The status is authoritative only
for this limited local register check. IRQ-disabled sections never use IOLog.

**Ownership limitation:** no PMU reservation protocol shared by all kernel,
firmware, hypervisor or external clients was established. The idle-register
check, local interrupt mask and disabled preemption do not prove exclusive
ownership against uncoordinated agents. Every runtime result says
`owner=uncoordinated`; restoring a snapshot could conflict with such a client.
Enable the diagnostic only when this limitation is acceptable. The worker's
online check also does not synchronize with a concurrent CPU hot-unplug. The
existing platform has unsupported secondary resume; a general hotplug-safe
binding protocol is not implemented. These paths have host-model tests but
have not been compiled as an XNU component or run on hardware. `idle=unknown`
is explicit: the forced busy interval is not an idle-residency measurement.
