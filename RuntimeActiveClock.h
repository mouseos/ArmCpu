// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "CycleBounds.h"

// The producer must hold a real platform PMU lease across every snapshot,
// verify access before reading PMU registers, and supply actual active/idle
// boundaries on the same CPU. This calculator obtains no such lease, does no
// register accesses, and does not prove what PMCCNTR counts during WFI or at
// other exception levels. Endpoint equality cannot detect transient changes.
struct ArmCpuActivePoint {
  uint64_t before, after, cycles, frequency, leaseGeneration, pmcr, enabled, filter, interrupts, overflow;
  unsigned cpu;
  bool leaseHeld, activeBoundaryKnown;
};
struct ArmCpuActiveClock {
  uint64_t generation, frequency, cycles, minTicks, maxTicks, lastAfter, lastCycles, pmcr, filter;
  unsigned cpu, intervals;
  bool open, invalid, haveLast;
  ArmCpuActivePoint start;
};
static inline bool armcpu_active_fail(ArmCpuActiveClock &s) { s.invalid = true; return false; }
static inline bool armcpu_active_point_valid(const ArmCpuActivePoint &p)
{
  return p.leaseHeld && p.activeBoundaryKnown && p.leaseGeneration && p.frequency >= 1000 &&
      p.frequency <= 1000000000 && p.cycles <= UINT32_MAX && p.overflow == 0 &&
      (p.pmcr & 0x7f) == 1 && p.enabled == (1ULL << 31) &&
      !(p.filter & 0xfc000000ULL) && p.interrupts == 0 && p.after - p.before <= UINT64_MAX / 2;
}
static inline bool armcpu_active_begin(ArmCpuActiveClock &s, const ArmCpuActivePoint &p)
{
  if (s.invalid || s.open || !armcpu_active_point_valid(p)) return armcpu_active_fail(s);
  if (s.intervals && (p.cpu != s.cpu || p.leaseGeneration != s.generation || p.frequency != s.frequency)) return armcpu_active_fail(s);
  if (s.haveLast && (p.before - s.lastAfter > UINT64_MAX / 2 || p.cycles < s.lastCycles ||
      p.pmcr != s.pmcr || p.filter != s.filter)) return armcpu_active_fail(s);
  if (!s.intervals) { s.cpu = p.cpu; s.generation = p.leaseGeneration; s.frequency = p.frequency; s.pmcr = p.pmcr; s.filter = p.filter; }
  s.start = p; s.open = true; return true;
}
static inline bool armcpu_active_end(ArmCpuActiveClock &s, const ArmCpuActivePoint &p)
{
  if (s.invalid || !s.open || !armcpu_active_point_valid(p)) return armcpu_active_fail(s);
  const ArmCpuActivePoint &b = s.start;
  if (p.cpu != s.cpu || p.leaseGeneration != s.generation || p.frequency != s.frequency ||
      p.pmcr != b.pmcr || p.enabled != b.enabled || p.filter != b.filter ||
      p.interrupts != b.interrupts || p.cycles <= b.cycles) return armcpu_active_fail(s);
  ArmCpuCycleBounds x = armcpu_cycle_bounds(p.cycles - b.cycles, s.frequency, b.before, b.after, p.before, p.after);
  if (!x.valid || UINT64_MAX - s.cycles < p.cycles - b.cycles ||
      UINT64_MAX - s.minTicks < x.minTicks || UINT64_MAX - s.maxTicks < x.maxTicks ||
      s.intervals == UINT32_MAX) return armcpu_active_fail(s);
  s.cycles += p.cycles - b.cycles; s.minTicks += x.minTicks; s.maxTicks += x.maxTicks;
  s.lastAfter = p.after; s.lastCycles = p.cycles; s.haveLast = true; s.open = false; ++s.intervals; return true;
}
// Historical aggregation validity only; it is not a current lease query.
static inline ArmCpuCycleBounds armcpu_active_result(const ArmCpuActiveClock &s)
{
  ArmCpuCycleBounds out = {};
  if (s.invalid || s.open || !s.intervals || !s.minTicks || !s.maxTicks || !s.frequency) return out;
  __uint128_t product = static_cast<__uint128_t>(s.cycles) * s.frequency;
  __uint128_t low = product / s.maxTicks, high = product / s.minTicks + (product % s.minTicks != 0);
  if (high > UINT64_MAX) return out;
  out.lowerHz = static_cast<uint64_t>(low); out.upperHz = static_cast<uint64_t>(high);
  out.minTicks = s.minTicks; out.maxTicks = s.maxTicks; out.valid = true; return out;
}
