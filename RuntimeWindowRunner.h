// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RuntimeClockWindow.h"
#ifndef ARMCPU_RUNTIME_TEST
extern "C" uint64_t unixnu_idle_epoch_current(unsigned);
extern "C" bool a04br3_pmu_lease_acquire(unsigned, uint64_t *);
extern "C" bool a04br3_pmu_lease_valid(unsigned, uint64_t);
extern "C" bool a04br3_pmu_lease_release(unsigned, uint64_t);
extern "C" bool a04br3_runtime_probe_end_checked(void);
#endif

// One experiment per boot at a time. If a probe is interrupted or reports
// interference this remains set, and the board watchdog gate remains held.
static volatile uint32_t armcpu_window_poison;
static bool armcpu_window_end_gate() {
  if (a04br3_runtime_probe_end_checked()) return true;
  __atomic_store_n(&armcpu_window_poison,1U,__ATOMIC_RELEASE);
  return false;
}
struct ArmCpuWindowLease {
  bool watchdogHeld() { return __atomic_load_n(&a04br3_runtime_probe_active,__ATOMIC_ACQUIRE)!=0; }
  bool valid(unsigned cpu, uint64_t generation) { return a04br3_pmu_lease_valid(cpu,generation); }
};
struct ArmCpuWindowRegisters : ArmCpuCycleRegisters {
  processor_t target; unsigned targetCpu;
  unsigned cpu() { return current_processor()==target ? targetCpu : ~0U; }
  uint64_t idleEpoch() { return unixnu_idle_epoch_current(targetCpu); }
};
static void armcpu_runtime_window_worker(void *, wait_result_t)
{
  IOSleep(5000);
  for (unsigned i=0;i<200 && !__atomic_load_n(&a04br3_runtime_recovery_ready,__ATOMIC_ACQUIRE);++i) {
    if (!ml_get_interrupts_enabled()) break;
    IOSleep(100);
  }
  if (!__atomic_load_n(&a04br3_runtime_recovery_ready,__ATOMIC_ACQUIRE) && ml_get_interrupts_enabled())
    IOLog("ArmCpu: runtime-window unavailable=recovery-not-ready\n");
  for(unsigned cpu=0;cpu<4;++cpu) {
    if (__atomic_load_n(&armcpu_window_poison,__ATOMIC_ACQUIRE)) break;
    if (!ml_get_interrupts_enabled()) break;
    if (!__atomic_load_n(&a04br3_runtime_recovery_ready,__ATOMIC_ACQUIRE)) continue;
    if (!armcpu_runtime_try_hotplug_lock()) {IOLog("ArmCpu: runtime-window cpu=%u unavailable=hotplug-busy\n",cpu);continue;}
    processor_t target=cpu_to_processor(static_cast<int>(cpu));
    if (armcpu_quiesce_calls || armcpu_halt_calls || (cpu && armcpu_smp_step[cpu]!=7) ||
        !target || !armcpu_runtime_online(target,cpu)) {armcpu_runtime_hotplug_unlock();IOLog("ArmCpu: runtime-window cpu=%u unavailable=offline-or-transition\n",cpu);continue;}
    if (!a04br3_runtime_probe_begin()) {armcpu_runtime_hotplug_unlock();IOLog("ArmCpu: runtime-window cpu=%u unavailable=watchdog-gate-busy\n",cpu);continue;}
    uint64_t deadline;clock_interval_to_deadline(250,1000000,&deadline);
    processor_t previous=thread_bind(target);thread_block(THREAD_CONTINUE_NULL);
    uint64_t resumed;clock_get_uptime(&resumed);
    if (resumed>=deadline || !a04br3_runtime_recovery_recheck()) {
      bool ended=armcpu_window_end_gate();thread_bind(previous);armcpu_runtime_hotplug_unlock();if (!ended) break;
      continue;
    }
    uint64_t generation=0;
    const bool acquired=a04br3_pmu_lease_acquire(cpu,&generation);
    if (acquired && !generation) {
      __atomic_store_n(&armcpu_window_poison,1U,__ATOMIC_RELEASE);
      thread_bind(previous);armcpu_runtime_hotplug_unlock();break;
    }
    if (!acquired) {
      bool ended=armcpu_window_end_gate();thread_bind(previous);armcpu_runtime_hotplug_unlock();
      if (ml_get_interrupts_enabled()) IOLog("ArmCpu: runtime-window cpu=%u unavailable=pmu-lease\n",cpu);
      if (!ended) break;
      continue;
    }
    ArmCpuWindowRegisters regs;regs.target=target;regs.targetCpu=cpu;regs.source=0;
    ArmCpuWindowLease lease;ArmCpuRuntimeWindow w={};ArmCpuRuntimeWindowResult result={};
    // Publish unresolved before the first possible PMU access. A trap or
    // non-return cannot accidentally permit a later probe in this boot.
    __atomic_store_n(&armcpu_window_poison,1U,__ATOMIC_RELEASE);
    disable_preemption();
    const uint64_t mpidr=regs.mpidr();
    if (current_processor()==target && (mpidr & 0xffffffULL)==cpu && ml_get_interrupts_enabled() && armcpu_timer_probe(regs,0).status==1)
      armcpu_runtime_window_begin(regs,lease,cpu,generation,w);
    else {w.status=10;w.cleanup=ArmCpuCleanupUntouched;}
    enable_preemption();
    bool recoveryStayed=true;
    if (w.started && w.status!=11) {
      // Other runnable threads can execute; the epoch rejects intervals that
      // enter machine_idle. This is not a tight synthetic busy loop.
      IOSleep(2);
      recoveryStayed=a04br3_runtime_recovery_recheck();
      disable_preemption();
      if (ml_get_interrupts_enabled()) result=armcpu_runtime_window_end(regs,lease,w);
      else {w.status=11;result.status=11;}
      enable_preemption();
    } else result.status=w.status;
    // IOSleep and rescheduling are not deadline guarantees. Refuse a window
    // longer than 50 ms after restoration rather than reporting stale data.
    if (!result.status && result.frequency && result.endAfter-result.beginBefore > result.frequency/20) {
      result.status=16;result.cycles=0;result.bounds=ArmCpuCycleBounds{};
    }
    bool safe=!w.started && recoveryStayed && (w.cleanup==ArmCpuCleanupUntouched || w.cleanup==ArmCpuCleanupRestored) && w.status!=11;
    bool leaseReleased=false, gateReleased=false;
    if (safe) {leaseReleased=a04br3_pmu_lease_release(cpu,generation);safe=leaseReleased;}
    if (safe) {gateReleased=armcpu_window_end_gate();safe=gateReleased;}
    if (safe) __atomic_store_n(&armcpu_window_poison,0U,__ATOMIC_RELEASE);
    thread_bind(previous);armcpu_runtime_hotplug_unlock();
    if (!safe) { if (!result.status) result.status=15; result.cycles=0; result.bounds=ArmCpuCycleBounds{}; }
    if (ml_get_interrupts_enabled()) {
      IOLog("ArmCpu: runtime-window cpu=%u mpidr=%llx status=%u cleanup=%u recovery=%u lease_released=%u gate_released=%u quarantine=%u cntfrq=%llu cycles=%llu low_hz=%llu high_hz=%llu idle=excluded irq=included\n",cpu,mpidr,result.status,static_cast<unsigned>(w.cleanup),static_cast<unsigned>(recoveryStayed),static_cast<unsigned>(leaseReleased),static_cast<unsigned>(gateReleased),static_cast<unsigned>(!safe),result.frequency,result.cycles,result.bounds.lowerHz,result.bounds.upperHz);
    }
    if (!safe) break;
  }
  thread_terminate(current_thread());
}
