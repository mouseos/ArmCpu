// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../CycleClock.h"
#include <cassert>

static void run(int startResult, bool irqBefore, bool irqAfter) {
  ArmCpuCycleStart start = {};
  ArmCpuCycleMailbox mailbox = {};
  ArmCpuCycleReportState state = {};
  int result = 0; unsigned before = 1, after = 1;
  ArmCpuCycleSample observed = {}; uint64_t mpidr = 0;
  assert(!start.read(result, before, after));
  assert(!mailbox.read(observed, mpidr));
  assert(state.pendingDue());
  start.publish(startResult, irqBefore, irqAfter);
  assert(start.read(result, before, after));
  assert(result == startResult && before == irqBefore && after == irqAfter);
  state.startReported = true;
  // More than five seconds of 100 ms polls must not retire the reporter.
  for (int i = 0; i < 120; ++i) {
    assert(!state.done());
    assert(!mailbox.read(observed, mpidr));
    bool due = state.pendingDue();
    assert(due == ((i + 1) % 50 == 0));
  }
  ArmCpuCycleSample sample = {}; sample.status = 7; sample.ticks = 42;
  mailbox.publish(sample, 0x80000102);
  assert(mailbox.read(observed, mpidr));
  assert(observed.status == 7 && observed.ticks == 42 && mpidr == 0x80000102);
  state.sampleReported = true;
  assert(state.done());
}

int main() {
  run(0, true, true);
  run(-1, true, true);
  run(0, false, false);
  run(-1000, false, false);
  // A sample may arrive while processor_start is still blocked.
  ArmCpuCycleReportState state = {};
  state.sampleReported = true;
  assert(!state.done());
  state.startReported = true;
  assert(state.done());
}
