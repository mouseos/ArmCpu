#include "../CycleClock.h"
#include <cassert>
#include <cstdio>

// Simulated elapsed time advances independently of the register being read.
// The PMU runs at 100 cycles per timer tick, including inside slow reads.
struct Independent {
  uint64_t clock=0, counter=123, control=8, flags=0;
  uint64_t delayFirst=3, delayLast=5, freq=1000000;
  unsigned reads=0;
  bool running=false;
  void advance(uint64_t ticks) {
    clock += ticks;
    if (running) {
      uint64_t next = counter + 100 * ticks;
      if (next > 0xffffffffULL) flags |= 1ULL << 31;
      counter = static_cast<uint32_t>(next);
    }
  }
  uint64_t id(){return 1ULL << 8;}
  uint64_t frequency(){return freq;}
  uint64_t pmcr(){return control;}
  uint64_t enabled(){return 0;}
  uint64_t filter(){return 0;}
  uint64_t interrupts(){return 0;}
  uint64_t overflow(){return flags;}
  uint64_t time(){advance(1); return clock;}
  uint64_t cycle(){if (running) advance(reads++ == 0 ? delayFirst : delayLast); return counter;}
  void set_cycle(uint64_t v){counter=v;}
  void set_pmcr(uint64_t v){control=v;}
  void enable_cycle(){running=true;}
  void disable_cycle(){running=false;}
  void clear_overflow(uint64_t v){flags &= ~v;}
  void barrier(){}
};
static void restored(const Independent &m) {
  assert(m.counter==123 && m.control==8 && !m.running && !m.flags);
}
int main() {
  Independent normal; auto a=armcpu_cycle_sample(normal);
  assert(a.status==0 && a.beginReadTicks==4 && a.endReadTicks==6);
  assert(a.outerTicks==1010 && a.ticks==1005 && a.cycles==100600);
  restored(normal);
  Independent first; first.delayFirst=2000; auto b=armcpu_cycle_sample(first);
  assert(b.status==6 && !b.cycles && b.beginReadTicks==2001 && b.outerTicks>2000);
  restored(first);
  Independent last; last.delayLast=2000; auto c=armcpu_cycle_sample(last);
  assert(c.status==6 && !c.cycles && c.endReadTicks==2001 && c.outerTicks>2000);
  restored(last);
  Independent boundary; boundary.delayFirst=499; boundary.delayLast=499;
  auto d=armcpu_cycle_sample(boundary);
  assert(d.outerTicks==2000 && d.status==8 && !d.cycles);
  restored(boundary);
  Independent beyond; beyond.delayFirst=500; beyond.delayLast=499;
  auto e=armcpu_cycle_sample(beyond);
  assert(e.outerTicks==2001 && e.status==6 && !e.cycles);
  restored(beyond);
  Independent combined; combined.delayFirst=4; combined.delayLast=5;
  auto f=armcpu_cycle_sample(combined);
  assert(f.outerTicks < 2000 && f.beginReadTicks==5 && f.endReadTicks==6 && f.status==8 && !f.cycles);
  restored(combined);
  Independent slowFirst; slowFirst.delayFirst=9; slowFirst.delayLast=0;
  auto g=armcpu_cycle_sample(slowFirst);
  assert(g.beginReadTicks==10 && g.endReadTicks==1 && g.outerTicks < 2000 && g.status==8 && !g.cycles);
  restored(slowFirst);
  Independent slowLast; slowLast.delayFirst=0; slowLast.delayLast=9;
  auto h=armcpu_cycle_sample(slowLast);
  assert(h.beginReadTicks==1 && h.endReadTicks==10 && h.outerTicks < 2000 && h.status==8 && !h.cycles);
  restored(slowLast);
  Independent fractional; fractional.freq=999999; fractional.delayFirst=3; fractional.delayLast=4;
  auto i=armcpu_cycle_sample(fractional);
  assert(i.beginReadTicks+i.endReadTicks==9 && i.status==0);
  restored(fractional);
  Independent rounded; rounded.freq=999999;
  auto j=armcpu_cycle_sample(rounded);
  assert(j.beginReadTicks+j.endReadTicks==10 && j.status==8 && !j.cycles);
  restored(rounded);
  puts("PASS: independent timer/PMU model, 1% read-width boundary, slow endpoints, 2 ms limit and restoration");
}
