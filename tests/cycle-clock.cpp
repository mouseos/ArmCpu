#include "../CycleClock.h"
#include <cassert>
#include <cstdio>
struct Fake {
  uint64_t ver=1, freq=13000000, control=8, enables=0, filters=0, counter=123, now=0;
  unsigned writes=0; bool running=false, stuck=false;
  uint64_t id(){return ver<<8;} uint64_t frequency(){return freq;} uint64_t pmcr(){return control;}
  uint64_t enabled(){return enables;} uint64_t filter(){return filters;} uint64_t cycle(){return counter;}
  uint64_t time(){if(!stuck){now+=100; if(running) counter+=10000;} return now;}
  void set_cycle(uint64_t x){counter=x; ++writes;} void set_pmcr(uint64_t x){control=x; ++writes;}
  void enable_cycle(){running=true; ++writes;} void disable_cycle(){running=false; ++writes;} void barrier(){}
};
int main(){
  Fake f; auto s=armcpu_cycle_sample(f); assert(!s.status && s.frequency==13000000 && s.ticks==13000 && s.cycles==1300000); assert(f.counter==123 && f.control==8 && !f.running);
  Fake n; n.ver=0; assert(armcpu_cycle_sample(n).status==1 && !n.writes);
  Fake t; t.freq=0; assert(armcpu_cycle_sample(t).status==2 && !t.writes);
  Fake p; p.control=1; assert(armcpu_cycle_sample(p).status==3 && !p.writes);
  Fake e; e.enables=1ULL<<31; assert(armcpu_cycle_sample(e).status==3 && !e.writes);
  Fake x; x.filters=1ULL<<31; assert(armcpu_cycle_sample(x).status==3 && !x.writes);
  Fake z; z.stuck=true; assert(armcpu_cycle_sample(z).status==4 && z.counter==123 && z.control==8 && !z.running);
  puts("PASS: measurement, restoration, refusal and timer timeout");
}
