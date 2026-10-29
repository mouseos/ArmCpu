#include "../RuntimeClockWindow.h"
#include <cassert>
#include <cstdio>
struct Lease {bool held=true,gate=true;unsigned calls=0,loseAt=0;bool watchdogHeld(){return gate;} bool valid(unsigned cpu,uint64_t gen){++calls;return held&&calls!=loseAt&&cpu==0&&gen==7;}};
struct Regs {
 unsigned cpuId=0, pmuReads=0, writes=0;bool lateOverflow=false,ignoreRestore=false,ignoreDisable=false,ignoreControl=false;unsigned restoreFault=0; uint64_t idle=0, now=100, value=123, control=8, enable=0, filt=0, intr=0, ov=0, freq=13000000, idvalue=1ULL<<8;
 unsigned cpu(){return cpuId;} uint64_t idleEpoch(){return idle;} uint64_t id(){return idvalue;} uint64_t frequency(){return freq;}
 uint64_t pmcr(){++pmuReads;return control;} uint64_t enabled(){++pmuReads;return enable;} uint64_t filter(){++pmuReads;return filt;}
 uint64_t interrupts(){++pmuReads;return intr;} uint64_t overflow(){++pmuReads;return ov;} uint64_t cycle(){++pmuReads;return value;}
 uint64_t time(){return now++;} void set_cycle(uint64_t x){++writes;if(!(ignoreRestore && x==123))value=x;if(x==123){if(restoreFault==1)filt=1;if(restoreFault==2)intr=1;if(restoreFault==3)ov=1;}} void set_pmcr(uint64_t x){++writes;if(!(ignoreControl&&x==8))control=x;}
 void enable_cycle(){++writes;enable=1ULL<<31;}void disable_cycle(){++writes;if(!ignoreDisable)enable=0;if(lateOverflow)ov|=1;}void clear_overflow(uint64_t x){++writes;ov&=~x;}void barrier(){}
};
struct FaultReadRegs : Regs { uint64_t id(){throw 43;} };
struct FaultRegs : Regs { void set_pmcr(uint64_t){throw 42;} };
int main(){
 Lease l;Regs r;ArmCpuRuntimeWindow w={};armcpu_runtime_window_begin(r,l,0,7,w);assert(w.started&&r.enable);r.now+=129;r.value=10000;
 auto x=armcpu_runtime_window_end(r,l,w);assert(x.status==0&&x.bounds.valid&&x.cycles==10000&&!w.started&&w.cleanup==ArmCpuCleanupRestored&&r.value==123&&r.control==8&&!r.enable);
 auto repeated=armcpu_runtime_window_end(r,l,w);assert(repeated.status==14&&!repeated.bounds.valid);
 FaultReadRegs readFault;bool readFaultSeen=false;try{w={};armcpu_runtime_window_begin(readFault,l,0,7,w);}catch(int){readFaultSeen=true;}assert(readFaultSeen&&l.gate&&w.started&&w.cleanup==ArmCpuCleanupUnknown&&!readFault.writes);
 FaultRegs fault;bool faultSeen=false;try{w={};armcpu_runtime_window_begin(fault,l,0,7,w);}catch(int){faultSeen=true;}assert(faultSeen&&l.gate&&fault.writes==1&&w.cleanup==ArmCpuCleanupUnknown&&w.started);
 Lease absent;absent.held=false;Regs a;ArmCpuRuntimeWindow n={};armcpu_runtime_window_begin(a,absent,0,7,n);assert(n.status==12&&!n.started&&n.cleanup==ArmCpuCleanupUntouched&&a.pmuReads==0&&a.writes==0);
 Lease noGate;noGate.gate=false;Regs noGateRegs;n={};armcpu_runtime_window_begin(noGateRegs,noGate,0,7,n);assert(n.status==12&&!noGateRegs.pmuReads&&!noGateRegs.writes);
 Regs invalidVersion;invalidVersion.idvalue=0;n={};armcpu_runtime_window_begin(invalidVersion,l,0,7,n);assert(n.status==1&&!n.started&&n.cleanup==ArmCpuCleanupUntouched&&!invalidVersion.writes);
 Regs invalidFreq;invalidFreq.freq=999;n={};armcpu_runtime_window_begin(invalidFreq,l,0,7,n);assert(n.status==2&&!n.started&&n.cleanup==ArmCpuCleanupUntouched&&!invalidFreq.writes);
 Regs busy;busy.control=1;n={};armcpu_runtime_window_begin(busy,l,0,7,n);assert(n.status==3&&!n.started&&n.cleanup==ArmCpuCleanupUntouched&&!busy.writes);
 Regs idle;idle.idle=1;n={};armcpu_runtime_window_begin(idle,l,0,7,n);assert(n.status==13&&!n.started&&!idle.pmuReads&&!idle.writes);
 Regs i;w={};armcpu_runtime_window_begin(i,l,0,7,w);i.idle=2;i.now+=129;i.value=10000;x=armcpu_runtime_window_end(i,l,w);assert(x.status==13&&!w.started&&i.value==123&&i.control==8);
 Regs conflict;w={};armcpu_runtime_window_begin(conflict,l,0,7,w);conflict.enable|=1;unsigned before=conflict.writes,reads=0;x=armcpu_runtime_window_end(conflict,l,w);assert(x.status==11&&w.started&&conflict.writes==before);conflict.enable=1ULL<<31;x=armcpu_runtime_window_end(conflict,l,w);assert(x.status==11&&conflict.writes==before);reads=conflict.pmuReads;armcpu_runtime_window_begin(conflict,l,0,7,w);assert(w.status==11&&w.cleanup==ArmCpuCleanupUnknown&&conflict.writes==before&&conflict.pmuReads==reads);
 Regs gone;w={};armcpu_runtime_window_begin(gone,l,0,7,w);l.held=false;before=gone.writes;reads=gone.pmuReads;x=armcpu_runtime_window_end(gone,l,w);assert(x.status==11&&gone.writes==before&&gone.pmuReads==reads);l.held=true;x=armcpu_runtime_window_end(gone,l,w);assert(x.status==11&&gone.writes==before&&gone.pmuReads==reads);
 Lease gateLoss;Regs ga;w={};armcpu_runtime_window_begin(ga,gateLoss,0,7,w);gateLoss.gate=false;before=ga.writes;reads=ga.pmuReads;x=armcpu_runtime_window_end(ga,gateLoss,w);assert(x.status==11&&ga.writes==before&&ga.pmuReads==reads);
 Lease revoke;Regs rev;w={};armcpu_runtime_window_begin(rev,revoke,0,7,w);revoke.loseAt=3;before=rev.writes;x=armcpu_runtime_window_end(rev,revoke,w);assert(x.status==11&&rev.writes==before);
 Regs migration;w={};armcpu_runtime_window_begin(migration,l,0,7,w);migration.cpuId=1;before=migration.writes;x=armcpu_runtime_window_end(migration,l,w);assert(x.status==11&&migration.writes==before);
 Regs freq;w={};armcpu_runtime_window_begin(freq,l,0,7,w);freq.freq=26000000;freq.now+=129;freq.value=10000;x=armcpu_runtime_window_end(freq,l,w);assert(x.status==9&&!x.bounds.valid&&!w.started&&freq.value==123&&freq.control==8&&freq.enable==0);
 Regs late;w={};armcpu_runtime_window_begin(late,l,0,7,w);late.lateOverflow=true;late.now+=129;late.value=10000;x=armcpu_runtime_window_end(late,l,w);assert(x.status==11&&w.started&&late.control==1&&late.value==10000);before=late.writes;reads=late.pmuReads;x=armcpu_runtime_window_end(late,l,w);assert(x.status==11&&late.writes==before&&late.pmuReads==reads);
 Regs restoreFail;w={};armcpu_runtime_window_begin(restoreFail,l,0,7,w);restoreFail.now+=129;restoreFail.value=10000;restoreFail.ignoreRestore=true;x=armcpu_runtime_window_end(restoreFail,l,w);assert(x.status==11&&w.started&&w.cleanup==ArmCpuCleanupUnknown&&restoreFail.value==10000);
 for(unsigned kind=0;kind<5;++kind){Regs badRestore;w={};armcpu_runtime_window_begin(badRestore,l,0,7,w);badRestore.now+=129;badRestore.value=10000;if(kind==0)badRestore.ignoreDisable=true;else if(kind==1)badRestore.ignoreControl=true;else badRestore.restoreFault=kind-1;x=armcpu_runtime_window_end(badRestore,l,w);assert(x.status==11&&!x.bounds.valid&&x.cycles==0&&w.cleanup==ArmCpuCleanupUnknown&&l.gate);before=badRestore.writes;reads=badRestore.pmuReads;x=armcpu_runtime_window_end(badRestore,l,w);assert(x.status==11&&badRestore.writes==before&&badRestore.pmuReads==reads);}
 Regs wrapped;w={};armcpu_runtime_window_begin(wrapped,l,0,7,w);wrapped.value=10000;wrapped.now+=129;wrapped.ov=1ULL<<31;x=armcpu_runtime_window_end(wrapped,l,w);assert(x.status==7&&!wrapped.ov&&wrapped.value==123);
 puts("PASS non-busy PMU transaction, idle refusal, conflict quarantine and restore");
}
