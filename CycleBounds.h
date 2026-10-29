// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// The cycle reads occur between beginBefore/beginAfter and
// endBefore/endAfter, respectively. These are conservative frequency bounds
// in nominal CNTFRQ units, not a calibration of the physical timer.
struct ArmCpuCycleBounds { uint64_t lowerHz, upperHz, minTicks, maxTicks; bool valid; };
static inline ArmCpuCycleBounds armcpu_cycle_bounds(uint64_t cycles, uint64_t frequency,
    uint64_t beginBefore, uint64_t beginAfter, uint64_t endBefore, uint64_t endAfter)
{
  ArmCpuCycleBounds out = {};
  uint64_t beginWidth = beginAfter - beginBefore;
  uint64_t middle = endBefore - beginAfter;
  uint64_t endWidth = endAfter - endBefore;
  // Reject backwards or ambiguous >= 2^63 intervals, including the sum.
  if (!cycles || !frequency || !middle || beginWidth > UINT64_MAX / 2 ||
      middle > UINT64_MAX / 2 || endWidth > UINT64_MAX / 2 ||
      beginWidth > UINT64_MAX / 2 - middle ||
      endWidth > UINT64_MAX / 2 - middle - beginWidth) return out;
  out.minTicks = middle;
  out.maxTicks = beginWidth + middle + endWidth;
  __uint128_t product = static_cast<__uint128_t>(cycles) * frequency;
  __uint128_t low = product / out.maxTicks;
  __uint128_t high = product / out.minTicks + (product % out.minTicks != 0);
  if (high > UINT64_MAX) return ArmCpuCycleBounds{};
  out.lowerHz = static_cast<uint64_t>(low);
  out.upperHz = static_cast<uint64_t>(high);
  out.valid = true;
  return out;
}
