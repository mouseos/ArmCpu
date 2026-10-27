#include "../CycleClock.h"
#include <cassert>
#include <cstdio>
struct Fake {
  uint64_t ver=1, freq=13000000, control=8, enables=0, filters=0, counter=123, now=0;
  uint64_t irq=0, flags=0, step=100, cycleStep=10000;
  unsigned writes=0; bool running=false, stuck=false, noCycles=false;
  uint64_t id(){return ver<<8;} uint64_t frequency(){return freq;} uint64_t pmcr(){return control;}
  uint64_t interrupts(){return irq;} uint64_t overflow(){return flags;}
  void clear_overflow(uint64_t x){flags &= ~x; ++writes;}
  uint64_t enabled(){return enables;} uint64_t filter(){return filters;} uint64_t cycle(){return counter;}
  uint64_t time(){if(!stuck){now+=step; if(running && !noCycles) { counter+=cycleStep; if(counter > 0xffffffffULL) { flags |= 1ULL<<31; counter &= 0xffffffffULL; } }} return now;}
  void set_cycle(uint64_t x){counter=x; ++writes;} void set_pmcr(uint64_t x){control=x; ++writes;}
  void enable_cycle(){running=true; ++writes;} void disable_cycle(){running=false; ++writes;} void barrier(){}
};
int main(){
  Fake f; auto s=armcpu_cycle_sample(f); assert(!s.status && s.frequency==13000000 && s.ticks==13000 && s.cycles==1300000); assert(f.counter==123 && f.control==8 && !f.running);
  Fake n; n.ver=0; assert(armcpu_cycle_sample(n).status==1 && !n.writes);
  Fake t; t.freq=0; assert(armcpu_cycle_sample(t).status==2 && !t.writes);
  Fake p; p.control=1; assert(armcpu_cycle_sample(p).status==3 && !p.writes);
  Fake e; e.enables=1ULL<<31; assert(armcpu_cycle_sample(e).status==3 && !e.writes);
  Fake other; other.enables=1; assert(armcpu_cycle_sample(other).status==3 && !other.writes);
  Fake x; x.filters=1ULL<<31; assert(armcpu_cycle_sample(x).status==3 && !x.writes);
  Fake irq; irq.irq=1; assert(armcpu_cycle_sample(irq).status==3 && !irq.writes);
  Fake overflow; overflow.flags=1ULL<<31; assert(armcpu_cycle_sample(overflow).status==3 && !overflow.writes && overflow.flags);
  Fake eventOverflow; eventOverflow.flags=1; assert(armcpu_cycle_sample(eventOverflow).status==3 && !eventOverflow.writes);
  Fake wrap; wrap.cycleStep=0x100000001ULL; auto ws=armcpu_cycle_sample(wrap); assert(ws.status==7 && !ws.cycles && ws.overflowAfter==(1ULL<<31) && !wrap.flags && wrap.counter==123 && wrap.control==8 && !wrap.running);
  Fake late; late.step=27000; auto ls=armcpu_cycle_sample(late); assert(ls.status==6 && !ls.cycles && late.counter==123 && !late.running);
  Fake zero; zero.noCycles=true; assert(armcpu_cycle_sample(zero).status==5 && zero.counter==123 && zero.control==8 && !zero.running);
  Fake reset; reset.control=14; assert(armcpu_cycle_sample(reset).status==0 && reset.control==8);
  Fake z; z.stuck=true; assert(armcpu_cycle_sample(z).status==4 && z.counter==123 && z.control==8 && !z.running);
  puts("PASS: measurement, restoration, interrupt/overflow refusal, wrap, elapsed limit and timeout");
}
