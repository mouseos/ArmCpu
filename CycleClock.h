// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// One calling CPU, before interrupts are enabled. Refuse an already owned PMU.
// The caller supplies ordered register operations; the result is raw so the
// timer frequency and both deltas remain independently inspectable.
struct ArmCpuCycleSample { uint64_t frequency, ticks, cycles, pmcr, filter, interrupts, overflow, overflowAfter, outerTicks, beginReadTicks, endReadTicks; unsigned status; };
template <typename R> static ArmCpuCycleSample armcpu_cycle_sample(R &r)
{
  ArmCpuCycleSample s = {};
  const uint64_t version = (r.id() >> 8) & 15;
  if (version == 0 || version == 15) { s.status = 1; return s; }
  s.frequency = r.frequency();
  if (s.frequency < 1000 || s.frequency > 1000000000) { s.status = 2; return s; }
  s.pmcr = r.pmcr();
  const uint64_t enabled = r.enabled();
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
  unsigned count = 0;
  const uint64_t target = s.frequency / 1000; // about one millisecond
  while ((endBefore - beginAfter) < target && count++ < 10000000) { endBefore = r.time(); }
  const uint64_t cyclesEnd = r.cycle();
  const uint64_t endAfter = r.time();
  r.disable_cycle();
  r.barrier();
  s.overflowAfter = r.overflow();
  if (s.overflowAfter & (1ULL << 31)) r.clear_overflow(1ULL << 31);
  r.set_pmcr(original);
  r.set_cycle(saved);
  r.barrier();
  s.beginReadTicks = beginAfter - beginBefore;
  s.endReadTicks = endAfter - endBefore;
  s.outerTicks = endAfter - beginBefore;
  // Midpoints estimate the timestamps of the two PMU reads. The full outer
  // interval bounds both reads and therefore also their true separation.
  s.ticks = (endBefore - beginBefore) + s.endReadTicks / 2 - s.beginReadTicks / 2;
  s.cycles = static_cast<uint32_t>(cyclesEnd - cyclesBegin);
  s.status = s.outerTicks > target * 2 ? 6 : (endBefore - beginAfter) < target ? 4 :
             (s.overflowAfter & (1ULL << 31)) ? 7 :
             s.beginReadTicks > target / 100 || s.endReadTicks > target / 100 - s.beginReadTicks ? 8 :
             s.cycles == 0 ? 5 : 0;
  if (s.status) s.cycles = 0;
  return s;
}
