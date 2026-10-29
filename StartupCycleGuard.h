// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "CycleClock.h"

// The board gate prevents the board's ordinary watchdog feeder from writing
// after begin() succeeds. A failed acquisition or recovery readback refuses
// PMU access. A detected conflict deliberately retains the gate: do not
// restore stale PMU state, and let the recovery watchdog attempt a reset.
// This does not control another watchdog feeder, higher EL, or a debugger.
template<typename Board, typename Registers>
static ArmCpuCycleSample armcpu_startup_guarded_sample(Board &board, Registers &registers)
{
  ArmCpuCycleSample sample = {};
  // Secondary CPUs can arrive together. Retry only for a bounded period;
  // a retained gate after interference must never be cleared by this CPU.
  if (!board.begin()) {
    const uint64_t frequency = registers.frequency();
    if (frequency < 1000 || frequency > 1000000000) { sample.status = 12; return sample; }
    const uint64_t start = registers.time(), limit = frequency / 100; // 10 ms
    uint64_t previous = start;
    bool acquired = false;
    for (unsigned i = 0; i < 1000000; ++i) {
      const uint64_t now = registers.time();
      if (now - previous > UINT64_MAX / 2 || now - start >= limit) break;
      previous = now;
      if (board.begin()) { acquired = true; break; }
    }
    if (!acquired) { sample.status = 12; return sample; }
  }
  if (!board.recoveryReady()) { board.end(); sample.status = 13; return sample; }
  sample = armcpu_cycle_sample(registers, 0, 0, true);
  if (sample.status != 11) board.end();
  return sample;
}
