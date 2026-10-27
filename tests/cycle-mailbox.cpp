// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../CycleClock.h"
#include <cassert>
#include <thread>

int main() {
  for (unsigned i = 0; i != 10000; ++i) {
    ArmCpuCycleMailbox mailbox = {};
    ArmCpuCycleSample observed = {};
    uint64_t mpidr = 0;
    assert(!mailbox.read(observed, mpidr));
    std::thread producer([&] {
      ArmCpuCycleSample value = {};
      value.frequency = 26000000;
      value.ticks = i + 1;
      value.cycles = (i + 1) * 123ULL;
      value.status = 8;
      value.endReadTicks = 99;
      mailbox.publish(value, 0x80000102ULL);
    });
    while (!mailbox.read(observed, mpidr)) std::this_thread::yield();
    assert(mpidr == 0x80000102ULL && observed.frequency == 26000000);
    assert(observed.ticks == i + 1 && observed.cycles == (i + 1) * 123ULL);
    assert(observed.status == 8 && observed.endReadTicks == 99);
    producer.join();
  }
}
