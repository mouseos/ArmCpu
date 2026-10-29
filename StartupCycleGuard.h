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
  if (!board.begin()) { sample.status = 12; return sample; }
  if (!board.recoveryReady()) { board.end(); sample.status = 13; return sample; }
  sample = armcpu_cycle_sample(registers, 0, 0, true);
  if (sample.status != 11) board.end();
  return sample;
}
