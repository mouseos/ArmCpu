// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../CycleClock.h"
#include <cassert>

int main() {
  int writes = 0;
  assert(!armcpu_report_if_irq(false, [&] { ++writes; }) && writes == 0);
  assert(armcpu_report_if_irq(true, [&] { ++writes; }) && writes == 1);
  ArmCpuCycleReportState state = {};
  assert(!state.done() && !state.expired(14999, 15000));
  assert(state.expired(15000, 15000));
  for (unsigned i = 0; i < 150; ++i) {
    assert(!state.expired(1, 15000));
    state.waited();
  }
  assert(state.expired(1, 15000)); // independent bound if the clock stalls
  ArmCpuCycleMailbox mailbox = {};
  ArmCpuCycleStart start = {};
  ArmCpuCycleSample observed = {}; uint64_t mpidr = 0;
  assert(!mailbox.read(observed, mpidr));
  ArmCpuCycleSample late = {}; late.status = 9; late.badPrevious = 100; late.badCurrent = 97;
  mailbox.publish(late, 0x80000001);
  assert(mailbox.read(observed, mpidr));
  assert(observed.status == 9 && observed.badPrevious == 100 && observed.badCurrent == 97 && mpidr == 0x80000001);
  start.publish(0, true, false);
  int result; unsigned before, after;
  assert(start.read(result, before, after) && result == 0 && before == 1 && after == 0);
  state.startReported = state.sampleReported = true;
  assert(state.done());
}
