# Runtime cycle diagnostics: blocked on ownership and CPU0 access

This branch contains a compile repair and a fail-closed safety guard. It does
not yet deliver the requested runtime CPU-clock measurements. The default-off
`armcpu-runtime-cycle=1` worker reports CPU0 as access-unverified and CPUs
1–3 as ownership-unverified. It does not bind a CPU or access the PMU because
no implementation of a platform-wide PMU lease has been established. The
host test supplies a fake owner solely to test the guarded branch.

The R6 generated XNU `config_cpu_counters.h` contained
`#define CONFIG_CPU_COUNTERS 0`. In that configuration `kpc_common.c` selects
its stub branch and `kpc_set_running` returns ENOTSUP. XNU's non-Apple ARM64
KPC implementation also states that PMUv3 is unsupported. XNU monotonic's
public integration is conditional on CONFIG_CPU_COUNTERS. These observations
rule out using this build's KPC as an ownership service; they do not rule out
EL2/EL3, debug, firmware, or other direct PMU users.

The fixed UEFI PlatformHelper sets HCR_EL2 and timer access when entering from
EL2, but does not configure MDCR_EL2 for the boot CPU; an EL1 entry skips the
EL2 path. The R5 direct PMU observations establish access only for secondary
CPUs in that particular startup interval. They do not prove CPU0 access,
current runtime permissions, absence of traps, or global ownership. The
identified stock TEE/ATF data remains encrypted in the available evidence.

The guarded code uses XNU's `processor_updown_lock` via a nonblocking try-lock
and opaque `processor_info` rather than dereferencing a private processor type.
XNU processor startup and shutdown use the same lock. The future enabled path
still requires review: processor_info's `running` includes transitional states,
and thread_bind followed by thread_block has no independently enforced
scheduling deadline. Neither property is demonstrated by the host mock.

To actually measure CPU0–3, implement and validate an opt-in ARM PMUv3 owner
in the appropriate XNU/platform component, including a lease shared with every
kernel PMU client and a defined agreement with higher-EL/debug owners. Establish
CPU0 and secondary PMU access from the actual EL2/EL3 boot chain (including
trap handling), and provide a hotplug-safe CPU rendezvous with a bounded abort.
Only then replace the fail-closed owner check, preserve/restore PMU state, and
verify the runtime sample and full kernel link. Even then the forced 0.9 ms
busy sample is not an idle-residency or workload-average measurement.
