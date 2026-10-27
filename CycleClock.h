// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// One calling CPU, before interrupts are enabled. Refuse an already owned PMU.
// The caller supplies ordered register operations; the result is raw so the
// timer frequency and both deltas remain independently inspectable.
struct ArmCpuCycleSample { uint64_t frequency, ticks, cycles, pmcr, filter, interrupts, overflow, overflowAfter; unsigned status; };
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
  const uint64_t begin = r.time();
  const uint64_t cyclesBegin = r.cycle();
  uint64_t end = begin;
  unsigned count = 0;
  const uint64_t target = s.frequency / 1000; // about one millisecond
  while ((end - begin) < target && count++ < 10000000) { end = r.time(); }
  const uint64_t cyclesEnd = r.cycle();
  r.disable_cycle();
  r.barrier();
  s.overflowAfter = r.overflow();
  if (s.overflowAfter & (1ULL << 31)) r.clear_overflow(1ULL << 31);
  r.set_pmcr(original);
  r.set_cycle(saved);
  r.barrier();
  s.ticks = end - begin;
  s.cycles = static_cast<uint32_t>(cyclesEnd - cyclesBegin);
  s.status = s.ticks < target ? 4 : s.ticks > target * 2 ? 6 :
             (s.overflowAfter & (1ULL << 31)) ? 7 : s.cycles == 0 ? 5 : 0;
  if (s.status) s.cycles = 0;
  return s;
}
