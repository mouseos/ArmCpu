// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "RuntimeClockWindow.h"
#ifndef ARMCPU_RUNTIME_ACTIVE
#define ARMCPU_RUNTIME_ACTIVE 0
#endif
#ifndef ARMCPU_RUNTIME_TEST
extern "C" uint64_t unixnu_idle_epoch_current(unsigned);
extern "C" bool unixnu_idle_epoch_observe(int *, uint64_t *, unsigned *, uint64_t *, uint64_t *, unsigned *);
#if ARMCPU_RUNTIME_ACTIVE
extern "C" bool unixnu_pmuv3_active_start(unsigned, uint64_t);
extern "C" bool unixnu_pmuv3_active_stop(unsigned, uint64_t, uint64_t *, uint64_t *, uint64_t *, unsigned *);
#endif
extern "C" bool a04br3_pmu_qualification_available(unsigned);
extern "C" bool a04br3_pmu_lease_acquire(unsigned, uint64_t *);
extern "C" bool a04br3_pmu_lease_valid(unsigned, uint64_t);
extern "C" bool a04br3_pmu_lease_release(unsigned, uint64_t);
extern "C" bool a04br3_runtime_probe_end_checked(void);
extern "C" void a04br3_cpu_context_sample(unsigned, unsigned, unsigned) __attribute__((weak_import));
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
#ifndef ARMCPU_RUNTIME_REPEAT_ROUNDS
#define ARMCPU_RUNTIME_REPEAT_ROUNDS 120U
#endif
#ifndef ARMCPU_RUNTIME_REPEAT_DELAY_MS
#define ARMCPU_RUNTIME_REPEAT_DELAY_MS 5000U
#endif
static void armcpu_runtime_window_worker(void *parameter, wait_result_t)
{
  const bool repeat = parameter != nullptr;
  const bool context = parameter == reinterpret_cast<void *>(2);
  const unsigned rounds = repeat ? ARMCPU_RUNTIME_REPEAT_ROUNDS : 1U;
  IOSleep(5000);
#if ARMCPU_RUNTIME_ACTIVE
  if (ml_get_interrupts_enabled()) IOLog("ArmCpu: runtime-scope machine-idle-excluded idle-irq-excluded ownership-unverified timer-uncalibrated\n");
#endif
  for (unsigned i=0;i<200 && !__atomic_load_n(&a04br3_runtime_recovery_ready,__ATOMIC_ACQUIRE);++i) {
    if (!ml_get_interrupts_enabled()) break;
    IOSleep(100);
  }
  if (!__atomic_load_n(&a04br3_runtime_recovery_ready,__ATOMIC_ACQUIRE) && ml_get_interrupts_enabled())
    IOLog("ArmCpu: runtime-window unavailable=recovery-not-ready\n");
  unsigned completedRounds=0;
  for (unsigned round=0;round<rounds;++round) {
  if (__atomic_load_n(&armcpu_window_poison,__ATOMIC_ACQUIRE) || !ml_get_interrupts_enabled()) break;
  if (round) IOSleep(ARMCPU_RUNTIME_REPEAT_DELAY_MS);
  for(unsigned cpu=0;cpu<4;++cpu) {
    if (__atomic_load_n(&armcpu_window_poison,__ATOMIC_ACQUIRE)) break;
    if (!ml_get_interrupts_enabled()) break;
    if (repeat) IOLog("ArmCpu: runtime-attempt round=%u cpu=%u\n",round,cpu);
    if (!__atomic_load_n(&a04br3_runtime_recovery_ready,__ATOMIC_ACQUIRE)) {if (repeat) IOLog("ArmCpu: runtime-skip cpu=%u reason=recovery-not-ready\n",cpu);continue;}
    // Advisory preflight only: acquisition must recheck qualification.
    if (!a04br3_pmu_qualification_available(cpu)) {IOLog("ArmCpu: runtime-window cpu=%u unavailable=pmu-unqualified\n",cpu);continue;}
    if (context && a04br3_cpu_context_sample) a04br3_cpu_context_sample(round,cpu,0);
    else if (context) IOLog("ArmCpu: runtime-context unavailable=board-hook\n");
    if (!armcpu_runtime_try_hotplug_lock()) {IOLog("ArmCpu: runtime-window cpu=%u unavailable=hotplug-busy\n",cpu);continue;}
    processor_t target=cpu_to_processor(static_cast<int>(cpu));
    if (armcpu_quiesce_calls || armcpu_halt_calls || (cpu && armcpu_smp_step[cpu]!=7) ||
        !target || !armcpu_runtime_online(target,cpu)) {armcpu_runtime_hotplug_unlock();IOLog("ArmCpu: runtime-window cpu=%u unavailable=offline-or-transition\n",cpu);continue;}
    if (!a04br3_runtime_probe_begin()) {armcpu_runtime_hotplug_unlock();IOLog("ArmCpu: runtime-window cpu=%u unavailable=watchdog-gate-busy\n",cpu);continue;}
    uint64_t deadline;clock_interval_to_deadline(250,1000000,&deadline);
    processor_t previous=thread_bind(target);thread_block(THREAD_CONTINUE_NULL);
    uint64_t resumed;clock_get_uptime(&resumed);
    if (resumed>=deadline || !a04br3_runtime_recovery_recheck()) {
      bool ended=armcpu_window_end_gate();thread_bind(previous);armcpu_runtime_hotplug_unlock();
      if (repeat && ml_get_interrupts_enabled()) IOLog("ArmCpu: runtime-skip cpu=%u reason=bind-deadline-or-recovery gate_released=%u\n",cpu,static_cast<unsigned>(ended));
      if (!ended) break;
      continue;
    }
    uint64_t generation=0;
    const bool acquired=a04br3_pmu_lease_acquire(cpu,&generation);
    if (acquired && !generation) {
      __atomic_store_n(&armcpu_window_poison,1U,__ATOMIC_RELEASE);
      thread_bind(previous);armcpu_runtime_hotplug_unlock();
      if (repeat && ml_get_interrupts_enabled()) IOLog("ArmCpu: runtime-skip cpu=%u reason=invalid-generation quarantine=1\n",cpu);
      break;
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
    int idleActual=-1; uint64_t idleRaw=UINT64_MAX,idleOld=0,idleSeen=0; unsigned idleReason=0,idleEntry=0; bool idleObserved=false;
    const uint64_t mpidr=regs.mpidr();
    if (current_processor()==target && (mpidr & 0xffffffULL)==cpu && ml_get_interrupts_enabled() && armcpu_timer_probe(regs,0).status==1)
      armcpu_runtime_window_begin(regs,lease,cpu,generation,w);
    else {w.status=10;w.cleanup=ArmCpuCleanupUntouched;}
    if (w.status==13) idleObserved=unixnu_idle_epoch_observe(&idleActual,&idleRaw,&idleReason,&idleOld,&idleSeen,&idleEntry);
#if ARMCPU_RUNTIME_ACTIVE
    bool activeStarted=false; uint64_t activeCycles=0,activeMin=0,activeMax=0,epochEnd=UINT64_MAX; unsigned activeIntervals=0;
    if (w.started && !w.status) {bool irq=ml_set_interrupts_enabled(false);activeStarted=unixnu_pmuv3_active_start(cpu,generation);ml_set_interrupts_enabled(irq);}
#endif
    enable_preemption();
    bool recoveryStayed=true;
    if (w.started && w.status!=11) {
      // Other runnable threads can execute; the epoch rejects intervals that
      // enter machine_idle. This is not a tight synthetic busy loop.
      IOSleep(repeat ? 10 : 2);
      recoveryStayed=a04br3_runtime_recovery_recheck();
      disable_preemption();
#if ARMCPU_RUNTIME_ACTIVE
      bool activeOk=false;
      if (activeStarted && ml_get_interrupts_enabled() && current_processor()==target) {
        bool irq=ml_set_interrupts_enabled(false);
        epochEnd=unixnu_idle_epoch_current(cpu);
        activeOk=unixnu_pmuv3_active_stop(cpu,generation,&activeCycles,&activeMin,&activeMax,&activeIntervals);
        ml_set_interrupts_enabled(irq);
      }
#endif
#if ARMCPU_RUNTIME_ACTIVE
      if (!activeStarted || !activeOk) {w.status=11;result.status=11;}
      else
#endif
      if (ml_get_interrupts_enabled()) result=armcpu_runtime_window_end(regs,lease,w);
      else {w.status=11;result.status=11;}
#if ARMCPU_RUNTIME_ACTIVE
      // Only override the whole-window idle rejection after the hardware has
      // been restored and every idle epoch has a matching pair of boundaries.
      if (activeStarted && activeOk && (result.status==0 || result.status==13) && w.cleanup==ArmCpuCleanupRestored &&
          epochEnd!=UINT64_MAX && !(epochEnd&1) && epochEnd>=w.idleEpoch && activeIntervals &&
          epochEnd-w.idleEpoch==2ULL*(activeIntervals-1) && activeMin && activeMax) {
        __uint128_t product=(__uint128_t)activeCycles*result.frequency;
        __uint128_t high=product/activeMin+(product%activeMin!=0);
        if (high<=UINT64_MAX) {
          result.cycles=activeCycles;result.bounds.lowerHz=(uint64_t)(product/activeMax);
          result.bounds.upperHz=(uint64_t)high;result.bounds.minTicks=activeMin;result.bounds.maxTicks=activeMax;
          result.bounds.valid=true;result.status=0;
        } else {result.status=9;result.cycles=0;result.bounds=ArmCpuCycleBounds{};}
      } else if (result.status==0) {result.status=13;result.cycles=0;result.bounds=ArmCpuCycleBounds{};}
#endif
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
    if (context && safe && a04br3_cpu_context_sample) a04br3_cpu_context_sample(round,cpu,1);
    if (!safe) { if (!result.status) result.status=15; result.cycles=0; result.bounds=ArmCpuCycleBounds{}; }
    if (ml_get_interrupts_enabled()) {
      if (result.status==13 && w.cleanup==ArmCpuCleanupUntouched) IOLog("ArmCpu: runtime-idle cpu=%u epoch=%llx actual=%d raw=%llx seen=%u reason=%u old=%llx cmp=%llx enter=%u\n",cpu,w.idleEpoch,idleActual,idleRaw,static_cast<unsigned>(idleObserved),idleReason,idleOld,idleSeen,idleEntry);
      if (repeat && result.status==0) IOLog("ArmCpu: runtime-time round=%u cpu=%u begin=%llu end=%llu timer_hz=%llu\n",round,cpu,result.beginBefore,result.endAfter,result.frequency);
      IOLog("ArmCpu: runtime-window cpu=%u mpidr=%llx status=%u cleanup=%u recovery=%u lease_released=%u gate_released=%u quarantine=%u cntfrq=%llu cycles=%llu min_ticks=%llu max_ticks=%llu intervals=%u low_hz=%llu high_hz=%llu\n",cpu,mpidr,result.status,static_cast<unsigned>(w.cleanup),static_cast<unsigned>(recoveryStayed),static_cast<unsigned>(leaseReleased),static_cast<unsigned>(gateReleased),static_cast<unsigned>(!safe),result.frequency,result.cycles,result.bounds.minTicks,result.bounds.maxTicks,
#if ARMCPU_RUNTIME_ACTIVE
            activeIntervals,
#else
            0U,
#endif
            result.bounds.lowerHz,result.bounds.upperHz);
    }
    if (!safe) break;
  }
  if (!__atomic_load_n(&armcpu_window_poison,__ATOMIC_ACQUIRE) && ml_get_interrupts_enabled()) ++completedRounds;
  }
  if (repeat && ml_get_interrupts_enabled()) IOLog("ArmCpu: runtime-finished rounds=%u reason=%s\n",completedRounds,__atomic_load_n(&armcpu_window_poison,__ATOMIC_ACQUIRE) ? "quarantine" : "complete");
  thread_terminate(current_thread());
}
