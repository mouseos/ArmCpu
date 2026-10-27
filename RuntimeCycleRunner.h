// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#ifndef ARMCPU_RUNTIME_TEST
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/cpu_data.h>
#endif

// This optional worker uses the R5 startup sampler only in a short, pinned,
// non-preemptible interval. Hardware-idle checks are not a system-wide PMU
// lease; all output explicitly says so. No logging occurs while IRQs are off.
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
  for (unsigned round = 0; round < 3; ++round) {
    for (unsigned cpu = 0; cpu < 4; ++cpu) {
      if (!ml_get_interrupts_enabled ()) break;
      processor_t target = cpu_to_processor(static_cast<int>(cpu));
      if (!target || (target->state != PROCESSOR_IDLE && target->state != PROCESSOR_DISPATCHING && target->state != PROCESSOR_RUNNING)) {
        ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=1\n", cpu, round);
        continue;
      }
      // The binding becomes effective after a block, not at thread_bind().
      processor_t previous = thread_bind(target);
      thread_block(THREAD_CONTINUE_NULL);
      ArmCpuCycleSample sample = {};
      uint64_t mpidr = 0;
      bool sameCpu = false, irqBefore = ml_get_interrupts_enabled ();
      if (irqBefore) {
        disable_preemption();
        bool oldIrq = ml_set_interrupts_enabled (false);
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
              sample = armcpu_cycle_sample(regs, targetTicks, limit);
            else { sample.frequency = frequency; sample.status = 2; }
          } else {
            sample.frequency = regs.frequency();
            sample.status = 10;
          }
          sameCpu = current_processor() == target;
        }
        ml_set_interrupts_enabled (oldIrq);
        enable_preemption();
      }
      thread_bind(previous);
      if (ml_get_interrupts_enabled ()) {
        if (!sameCpu || !irqBefore) {
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u unavailable=2 irq_before=%u\n", cpu, round, static_cast<unsigned>(irqBefore));
        } else {
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle cpu=%u round=%u mpidr=%llx status=%u cntfrq=%llu\n", cpu, round, mpidr, sample.status, sample.frequency);
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-count cpu=%u ticks=%llu cycles=%llu outer=%llu\n", cpu, sample.ticks, sample.cycles, sample.outerTicks);
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-pmu cpu=%u pmcr=%llx enabled=%llx filter=%llx interrupts=%llx overflow=%llx after=%llx\n", cpu, sample.pmcr, sample.enabledBefore, sample.filter, sample.interrupts, sample.overflow, sample.overflowAfter);
          ARMCPU_RUNTIME_LOG ("ArmCpu: runtime-cycle-window cpu=%u begin=%llu end=%llu busy=1 idle=unknown owner=uncoordinated\n", cpu, sample.beginReadTicks, sample.endReadTicks);
        }
      }
    }
    if (round != 2 && ml_get_interrupts_enabled ()) IOSleep (1000);
  }
#undef ARMCPU_RUNTIME_LOG
  if (!ml_get_interrupts_enabled ()) ml_set_interrupts_enabled (true);
  thread_terminate(current_thread());
}
