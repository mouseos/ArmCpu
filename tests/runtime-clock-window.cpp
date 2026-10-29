#include "../RuntimeClockWindow.h"
#include <cassert>
#include <cstdio>
struct Lease {bool held=true,gate=true;unsigned calls=0,loseAt=0;bool watchdogHeld(){return gate;} bool valid(unsigned cpu,uint64_t gen){++calls;return held&&calls!=loseAt&&cpu==0&&gen==7;}};
struct Regs {
 unsigned cpuId=0, pmuReads=0, writes=0;bool lateOverflow=false; uint64_t idle=0, now=100, value=123, control=8, enable=0, filt=0, intr=0, ov=0, freq=13000000;
 unsigned cpu(){return cpuId;} uint64_t idleEpoch(){return idle;} uint64_t id(){return 1ULL<<8;} uint64_t frequency(){return freq;}
 uint64_t pmcr(){++pmuReads;return control;} uint64_t enabled(){++pmuReads;return enable;} uint64_t filter(){++pmuReads;return filt;}
 uint64_t interrupts(){++pmuReads;return intr;} uint64_t overflow(){++pmuReads;return ov;} uint64_t cycle(){++pmuReads;return value;}
 uint64_t time(){return now++;} void set_cycle(uint64_t x){++writes;value=x;} void set_pmcr(uint64_t x){++writes;control=x;}
 void enable_cycle(){++writes;enable=1ULL<<31;}void disable_cycle(){++writes;enable=0;if(lateOverflow)ov|=1;}void clear_overflow(uint64_t x){++writes;ov&=~x;}void barrier(){}
};
struct FaultRegs : Regs { void set_pmcr(uint64_t){throw 42;} };
int main(){
 Lease l;Regs r;auto w=armcpu_runtime_window_begin(r,l,0,7);assert(w.started&&r.enable);r.now+=129;r.value=10000;
 auto x=armcpu_runtime_window_end(r,l,w);assert(x.status==0&&x.bounds.valid&&x.cycles==10000&&!w.started&&r.value==123&&r.control==8&&!r.enable);
 auto repeated=armcpu_runtime_window_end(r,l,w);assert(repeated.status==14&&!repeated.bounds.valid);
 FaultRegs fault;bool faultSeen=false;try{(void)armcpu_runtime_window_begin(fault,l,0,7);}catch(int){faultSeen=true;}assert(faultSeen&&l.gate&&fault.writes==1);
 Lease absent;absent.held=false;Regs a;auto n=armcpu_runtime_window_begin(a,absent,0,7);assert(n.status==12&&!n.started&&a.pmuReads==0&&a.writes==0);
 Lease noGate;noGate.gate=false;Regs noGateRegs;n=armcpu_runtime_window_begin(noGateRegs,noGate,0,7);assert(n.status==12&&!noGateRegs.pmuReads&&!noGateRegs.writes);
 Regs idle;idle.idle=1;n=armcpu_runtime_window_begin(idle,l,0,7);assert(n.status==13&&!n.started&&!idle.pmuReads&&!idle.writes);
 Regs i;w=armcpu_runtime_window_begin(i,l,0,7);i.idle=2;i.now+=129;i.value=10000;x=armcpu_runtime_window_end(i,l,w);assert(x.status==13&&!w.started&&i.value==123&&i.control==8);
 Regs conflict;w=armcpu_runtime_window_begin(conflict,l,0,7);conflict.enable|=1;unsigned before=conflict.writes;x=armcpu_runtime_window_end(conflict,l,w);assert(x.status==11&&w.started&&conflict.writes==before);conflict.enable=1ULL<<31;x=armcpu_runtime_window_end(conflict,l,w);assert(x.status==11&&conflict.writes==before);
 Regs gone;w=armcpu_runtime_window_begin(gone,l,0,7);l.held=false;before=gone.writes;unsigned reads=gone.pmuReads;x=armcpu_runtime_window_end(gone,l,w);assert(x.status==11&&gone.writes==before&&gone.pmuReads==reads);l.held=true;x=armcpu_runtime_window_end(gone,l,w);assert(x.status==11&&gone.writes==before&&gone.pmuReads==reads);
 Lease gateLoss;Regs ga;w=armcpu_runtime_window_begin(ga,gateLoss,0,7);gateLoss.gate=false;before=ga.writes;reads=ga.pmuReads;x=armcpu_runtime_window_end(ga,gateLoss,w);assert(x.status==11&&ga.writes==before&&ga.pmuReads==reads);
 Lease revoke;Regs rev;w=armcpu_runtime_window_begin(rev,revoke,0,7);revoke.loseAt=3;before=rev.writes;x=armcpu_runtime_window_end(rev,revoke,w);assert(x.status==11&&rev.writes==before);
 Regs migration;w=armcpu_runtime_window_begin(migration,l,0,7);migration.cpuId=1;before=migration.writes;x=armcpu_runtime_window_end(migration,l,w);assert(x.status==11&&migration.writes==before);
 Regs freq;w=armcpu_runtime_window_begin(freq,l,0,7);freq.freq=26000000;freq.now+=129;freq.value=10000;x=armcpu_runtime_window_end(freq,l,w);assert(x.status==9&&!x.bounds.valid&&!w.started&&freq.value==123&&freq.control==8&&freq.enable==0);
 Regs late;w=armcpu_runtime_window_begin(late,l,0,7);late.lateOverflow=true;late.now+=129;late.value=10000;x=armcpu_runtime_window_end(late,l,w);assert(x.status==11&&w.started&&late.control==1&&late.value==10000);before=late.writes;reads=late.pmuReads;x=armcpu_runtime_window_end(late,l,w);assert(x.status==11&&late.writes==before&&late.pmuReads==reads);
 Regs wrapped;w=armcpu_runtime_window_begin(wrapped,l,0,7);wrapped.value=10000;wrapped.now+=129;wrapped.ov=1ULL<<31;x=armcpu_runtime_window_end(wrapped,l,w);assert(x.status==7&&!wrapped.ov&&wrapped.value==123);
 puts("PASS non-busy PMU transaction, idle refusal, conflict quarantine and restore");
}
