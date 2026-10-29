// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "CycleBounds.h"
#ifndef ARMCPU_RUNTIME_TEST
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/cpu_data.h>
#include <kern/locks.h>
#include <mach/processor_info.h>
// These are XNU-internal kernel entry points. The exported IOKit headers
// deliberately omit their declarations; signatures match the fixed XNU tree.
extern "C" processor_t cpu_to_processor(int);
extern "C" processor_t current_processor(void);
extern "C" processor_t thread_bind(processor_t);
extern "C" kern_return_t processor_info(processor_t, processor_flavor_t, host_t *, processor_info_t, mach_msg_type_number_t *);
extern "C" lck_mtx_t processor_updown_lock;
extern "C" volatile uint32_t a04br3_runtime_recovery_ready, a04br3_runtime_probe_active;
extern "C" bool a04br3_runtime_recovery_recheck(void);
extern "C" bool a04br3_runtime_probe_begin(void);
extern "C" void a04br3_runtime_probe_end(void);
static bool armcpu_runtime_online(processor_t p, unsigned cpu) {
  processor_basic_info_data_t info = {};
  mach_msg_type_number_t count = PROCESSOR_BASIC_INFO_COUNT;
  host_t host = nullptr;
  return processor_info(p, PROCESSOR_BASIC_INFO, &host, reinterpret_cast<processor_info_t>(&info), &count) == KERN_SUCCESS && info.running && info.slot_num == static_cast<int>(cpu);
}
static bool armcpu_runtime_try_hotplug_lock() { return lck_mtx_try_lock(&processor_updown_lock); }
static void armcpu_runtime_hotplug_unlock() { lck_mtx_unlock(&processor_updown_lock); }
#else
// The host harness supplies the policy and lock shims.
#endif

// This optional worker uses the R5 startup sampler only in a short, pinned,
// non-preemptible interval with interrupts enabled. Hardware-idle checks
// are not a system-wide PMU lease; all output explicitly says so.
static void armcpu_runtime_cycle_worker(void *, wait_result_t)
{
  // Kernel thread continuations normally begin with interrupts enabled.
  // Recover the standalone worker before terminating if that invariant fails.
  if (!ml_get_interrupts_enabled ()) {
    ml_set_interrupts_enabled (true);
    thread_terminate(current_thread());
    return;
  }
#define ARMCPU_RUNTIME_LOG(...) do { if (ml_get_interrupts_enabled ()) IOLog (__VA_ARGS__); } while (0)
  IOSleep (5000);
  // Board initialization occurs later than CPU registration. Wait finitely for
  // the verified recovery flag; never touch PMU if it never becomes ready.
  for (unsigned n = 0; n < 200 && !__atomic_load_n(&a04br3_runtime_recovery_ready, __ATOMIC_ACQUIRE); ++n) {
    if (!ml_get_interrupts_enabled ()) break;
    IOSleep (100);
  }
  for (unsigned round = 0; round < 3; ++round) {
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
      if (!ml_get_interrupts_enabled ()) break;
      // Refuse before touching recovery state, binding, or any PMU register.
      if (!armcpu_runtime_policy_allows(cpu)) {
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=ownership-or-access-unverified\n", cpu, round);
        continue;
      }
      // The boot CPU has not been probed before. The board's verified
      // fastboot flag and armed watchdog are mandatory for every PMU probe.
      if (!__atomic_load_n(&a04br3_runtime_recovery_ready, __ATOMIC_ACQUIRE)) {
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=recovery-not-ready\n", cpu, round);
        continue;
      }
      if (!armcpu_runtime_try_hotplug_lock()) {
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=hotplug-busy\n", cpu, round);
        continue;
      }
      // This device does not support CPU resume. Refuse the experiment if
      // any quiesce/halt has been observed; do not mistake a temporary
      // shutdown reported as running by processor_info for a usable CPU.
      bool powerTransition = armcpu_quiesce_calls != 0 || armcpu_halt_calls != 0;
      processor_t target = cpu_to_processor(static_cast<int>(cpu));
      bool startupComplete = cpu == 0 || armcpu_smp_step[cpu] == 7;
      if (powerTransition || !startupComplete || !target || !armcpu_runtime_online(target, cpu)) {
        armcpu_runtime_hotplug_unlock();
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=1\n", cpu, round);
        continue;
      }
      // The binding becomes effective after a block, not at thread_bind().
      uint64_t deadline;
      clock_interval_to_deadline (250, 1000000, &deadline);
      // Keep fastboot WDT effective even if the bound worker never resumes.
      if (!a04br3_runtime_probe_begin()) {
        armcpu_runtime_hotplug_unlock();
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=watchdog-gate-busy\n", cpu, round);
        continue;
      }
      processor_t previous = thread_bind(target);
      thread_block(THREAD_CONTINUE_NULL);
      uint64_t resumed;
      clock_get_uptime (&resumed);
      if (resumed >= deadline) {
        a04br3_runtime_probe_end();
        thread_bind(previous);
        armcpu_runtime_hotplug_unlock();
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=bind-deadline\n", cpu, round);
        continue;
      }
      // Re-read RTC and watchdog after binding, with IRQs enabled. The board
      // returns false on contention or a failed read; this is not an EL lease.
      if (!a04br3_runtime_recovery_recheck()) {
        a04br3_runtime_probe_end();
        thread_bind(previous);
        armcpu_runtime_hotplug_unlock();
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=recovery-recheck-failed\n", cpu, round);
        continue;
      }
      ArmCpuCycleSample sample = {};
      uint64_t mpidr = 0;
      bool sameCpu = false, irqBefore = ml_get_interrupts_enabled ();
      if (irqBefore) {
        // Keep local interrupts enabled. The pinned sample includes interrupt
        // activity, and has no guarantee that another PMU owner stays away.
        disable_preemption();
        sameCpu = current_processor() == target;
        if (sameCpu) {
          ArmCpuCycleRegisters regs;
          mpidr = regs.mpidr();
          ArmCpuTimerProbe virtualTimer = armcpu_timer_probe(regs, 0);
          if (virtualTimer.status == 1) {
            regs.source = 0;
            // Reserve 10% for PMU-read brackets and refuse an outer window
            // longer than 1 ms; zero is never passed as a custom target.
            uint64_t frequency = regs.frequency();
            uint64_t limit = frequency / 1000;
            uint64_t targetTicks = limit - limit / 10;
            if (frequency >= 1000 && frequency <= 1000000000 && targetTicks)
              sample = armcpu_cycle_sample(regs, targetTicks, limit, true);
            else { sample.frequency = frequency; sample.status = 2; }
          } else {
            sample.frequency = regs.frequency();
            sample.status = 10;
          }
          sameCpu = current_processor() == target;
        }
        enable_preemption();
      }
      // On detected interference do not resume feeding the watchdog: the
      // PMU may still be enabled and belongs to an unknown actor. Preserve its
      // state and let the recovery watchdog attempt to return to fastboot.
      if (sample.status != 11)
        a04br3_runtime_probe_end();
      thread_bind(previous);
      armcpu_runtime_hotplug_unlock();
      if (ml_get_interrupts_enabled ()) {
        if (!sameCpu || !irqBefore) {
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=2 irq_before=%u\n", cpu, round, static_cast<unsigned>(irqBefore));
        } else {
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u mpidr=%llx status=%u cntfrq=%llu\n", cpu, round, mpidr, sample.status, sample.frequency);
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-count cpu=%u ticks=%llu cycles=%llu outer=%llu\n", cpu, sample.ticks, sample.cycles, sample.outerTicks);
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-pmu cpu=%u pmcr=%llx enabled=%llx filter=%llx interrupts=%llx overflow=%llx after=%llx\n", cpu, sample.pmcr, sample.enabledBefore, sample.filter, sample.interrupts, sample.overflow, sample.overflowAfter);
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-window cpu=%u begin=%llu end=%llu busy=1 idle=unknown irq=enabled owner=unverified-kpc-disabled\n", cpu, sample.beginReadTicks, sample.endReadTicks);
          if (!sample.status) {
            ArmCpuCycleBounds bounds = armcpu_cycle_bounds(sample.cycles, sample.frequency, sample.beginBefore, sample.beginAfter, sample.endBefore, sample.endAfter);
            if (bounds.valid) ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-bounds cpu=%u low_hz=%llu high_hz=%llu min_ticks=%llu max_ticks=%llu timer_calibration=unknown\n", cpu, bounds.lowerHz, bounds.upperHz, bounds.minTicks, bounds.maxTicks);
            else ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-bounds cpu=%u unavailable=invalid-interval\n", cpu);
          }
        }
      }
      if (sample.status == 11) {
        // An observed PMU configuration change means ownership is unknown.
        // Never perform another probe in this boot after such a conflict.
        thread_terminate(current_thread());
        return;
      }
    }
    if (round != 2 && ml_get_interrupts_enabled ()) IOSleep (1000);
  }
#undef ARMCPU_RUNTIME_LOG
  if (!ml_get_interrupts_enabled ()) ml_set_interrupts_enabled (true);
  thread_terminate(current_thread());
}
