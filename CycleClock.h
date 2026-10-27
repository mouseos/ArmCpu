// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Probe before claiming that a counter can time a measurement. A small
// bounded forward step permits normal 64-bit rollover but rejects regression.
struct ArmCpuTimerProbe { uint64_t first, last; unsigned status; };
template <typename R> static ArmCpuTimerProbe armcpu_timer_probe(R &r, unsigned source)
{
  ArmCpuTimerProbe p = {}; p.first = p.last = r.counter(source);
  for (unsigned i = 0; i < 1024; ++i) {
    uint64_t next = r.counter(source), delta = next - p.last;
    if (delta > UINT64_MAX / 2) { p.last = next; p.status = 2; return p; }
    p.last = next;
    if (p.last != p.first) { p.status = 1; return p; }
  }
  return p; // stopped or too slow to observe within the bounded probe
}

// State 3 means the physical counter was not accessed. Only probe it when
// the virtual counter fails its bounded liveness check.
template <typename R> static void armcpu_probe_timer_sources(R &r, ArmCpuTimerProbe &v, ArmCpuTimerProbe &p)
{
  v = armcpu_timer_probe(r, 0);
  p = {};
  p.status = 3;
  if (v.status != 1) p = armcpu_timer_probe(r, 1);
}

// 0 means no qualified source; 1 is virtual, 2 physical. Do not select a
// stalled or backward source, even if it has a nonzero register value.
static inline unsigned armcpu_timer_select(const ArmCpuTimerProbe &v, const ArmCpuTimerProbe &p)
{
  return v.status == 1 ? 1 : p.status == 1 ? 2 : 0;
}

// One calling CPU, before interrupts are enabled. Refuse an already owned PMU.
// The caller supplies ordered register operations; the result is raw so the
// timer frequency and both deltas remain independently inspectable.
struct ArmCpuCycleSample { uint64_t frequency, ticks, cycles, pmcr, enabledBefore, filter, interrupts, overflow, overflowAfter, outerTicks, beginReadTicks, endReadTicks, beginBefore, beginAfter, endBefore, endAfter, badPrevious, badCurrent; unsigned status, badStage, timeSource; ArmCpuTimerProbe virtualBefore, virtualAfter, physicalAfter; uint64_t timerControlBefore, timerControlAfter, timerAccess; };

// Single producer (the secondary) and single consumer (its reporter thread).
// Publish only once, after every field including the actual MPIDR is stored.
struct ArmCpuCycleMailbox {
  ArmCpuCycleSample sample;
  uint64_t mpidr;
  unsigned ready;
  void publish(const ArmCpuCycleSample &value, uint64_t cpuMpidr) {
    sample = value;
    mpidr = cpuMpidr;
    __atomic_store_n(&ready, 1U, __ATOMIC_RELEASE);
  }
  bool read(ArmCpuCycleSample &value, uint64_t &cpuMpidr) const {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return false;
    value = sample;
    cpuMpidr = mpidr;
    return true;
  }
};

// The startup worker publishes independently of the sampled CPU, including
// the interrupt state that formerly caused silent loss of the report.
struct ArmCpuCycleStart {
  int result;
  unsigned irqBefore, irqAfter, ready;
  void publish(int value, bool before, bool after) {
    result = value; irqBefore = before; irqAfter = after;
    __atomic_store_n(&ready, 1U, __ATOMIC_RELEASE);
  }
  bool read(int &value, unsigned &before, unsigned &after) const {
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) return false;
    value = result; before = irqBefore; after = irqAfter;
    return true;
  }
};

// The callback is never invoked with interrupts disabled. Callers may record
// the failure without consuming the published mailbox contents.
template <typename F> static bool armcpu_report_if_irq(bool enabled, F log)
{
  if (!enabled) return false;
  log();
  return true;
}

// Reporter-owned state: pending messages never retire either input.
struct ArmCpuCycleReportState {
  bool startReported, sampleReported;
  unsigned waits;
  bool done() const { return startReported && sampleReported; }
  bool expired(uint64_t now, uint64_t deadline) const { return now >= deadline || waits >= 150; }
  void waited() { ++waits; }
};
template <typename R> static ArmCpuCycleSample armcpu_cycle_sample(R &r, uint64_t targetTicks = 0, uint64_t maxOuterTicks = 0)
{
  ArmCpuCycleSample s = {};
  const uint64_t version = (r.id() >> 8) & 15;
  if (version == 0 || version == 15) { s.status = 1; return s; }
  s.frequency = r.frequency();
  if (s.frequency < 1000 || s.frequency > 1000000000) { s.status = 2; return s; }
  s.pmcr = r.pmcr();
  const uint64_t enabled = r.enabled();
  s.enabledBefore = enabled;
  s.filter = r.filter();
  s.interrupts = r.interrupts();
  s.overflow = r.overflow();
  if ((s.pmcr & 1) || enabled || (s.filter & 0xfc000000ULL) || s.interrupts || s.overflow) { s.status = 3; return s; }
  const uint64_t saved = r.cycle();
  // P and C are write-one reset bits. Clear D (divide by 64); never reset
  // other counters. Restore the original count and configuration afterwards.
  const uint64_t original = s.pmcr & ~6ULL;
  r.set_cycle(0);
  r.set_pmcr((original & ~(8ULL | 64ULL)) | 1);
  r.enable_cycle();
  r.barrier();
  const uint64_t beginBefore = r.time();
  const uint64_t cyclesBegin = r.cycle();
  const uint64_t beginAfter = r.time();
  uint64_t endBefore = beginAfter;
  uint64_t previous = beginAfter;
  bool invalidTime = false;
  unsigned count = 0, unchanged = 0;
  const uint64_t target = targetTicks ? targetTicks : s.frequency / 1000;
  const uint64_t outerLimit = maxOuterTicks ? maxOuterTicks : target * 2;
  // A small backward jump becomes a huge unsigned delta. Reject it before
  // accepting it as a completed interval; modular wrap remains supported.
  invalidTime = (beginAfter - beginBefore) > UINT64_MAX / 2;
  if (invalidTime) { s.badPrevious = beginBefore; s.badCurrent = beginAfter; s.badStage = 1; }
  while (!invalidTime && (endBefore - beginAfter) < target && count++ < 262144 && unchanged < 4096) {
    endBefore = r.time();
    if (endBefore - previous > UINT64_MAX / 2) {
      invalidTime = true; s.badPrevious = previous; s.badCurrent = endBefore; s.badStage = 2;
    }
    unchanged = endBefore == previous ? unchanged + 1 : 0;
    previous = endBefore;
  }
  const uint64_t cyclesEnd = r.cycle();
  const uint64_t endAfter = r.time();
  r.disable_cycle();
  r.barrier();
  s.overflowAfter = r.overflow();
  if (s.overflowAfter & (1ULL << 31)) r.clear_overflow(1ULL << 31);
  r.set_pmcr(original);
  r.set_cycle(saved);
  r.barrier();
  s.beginBefore = beginBefore; s.beginAfter = beginAfter;
  s.endBefore = endBefore; s.endAfter = endAfter;
  if (!invalidTime && endAfter - endBefore > UINT64_MAX / 2) {
    invalidTime = true; s.badPrevious = endBefore; s.badCurrent = endAfter; s.badStage = 3;
  }
  s.beginReadTicks = beginAfter - beginBefore;
  s.endReadTicks = endAfter - endBefore;
  s.outerTicks = endAfter - beginBefore;
  // Midpoints estimate the timestamps of the two PMU reads. The full outer
  // interval bounds both reads and therefore also their true separation.
  if (!invalidTime)
    s.ticks = (endBefore - beginBefore) + s.endReadTicks / 2 - s.beginReadTicks / 2;
  s.cycles = static_cast<uint32_t>(cyclesEnd - cyclesBegin);
  s.status = invalidTime ? 9 : s.outerTicks > outerLimit ? 6 : (endBefore - beginAfter) < target ? 4 :
             (s.overflowAfter & (1ULL << 31)) ? 7 :
             s.beginReadTicks > target / 100 || s.endReadTicks > target / 100 - s.beginReadTicks ? 8 :
             s.cycles == 0 ? 5 : 0;
  if (s.status) s.cycles = 0;
  return s;
}
