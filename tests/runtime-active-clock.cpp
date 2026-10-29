#include "../RuntimeActiveClock.h"
#include <cassert>
#include <cstdio>
static ArmCpuActivePoint point(uint64_t before,uint64_t after,uint64_t cycles) {
  return {before,after,cycles,13000000,7,1,1ULL<<31,0,0,0,0,true,true};
}
int main() {
  ArmCpuActiveClock s = {}; auto a=point(100,102,1000),b=point(230,232,11000);
  assert(armcpu_active_begin(s,a));assert(armcpu_active_end(s,b));
  auto c=point(300,302,15000),d=point(430,432,25000);
  assert(armcpu_active_begin(s,c));assert(armcpu_active_end(s,d));
  auto x=armcpu_active_result(s);assert(x.valid && x.minTicks==256 && x.maxTicks==264 && s.cycles==20000);
  assert(x.lowerHz==984848484 && x.upperHz==1015625000);
  ArmCpuActiveClock cpu={};auto other=b;other.cpu=1;assert(armcpu_active_begin(cpu,a));assert(!armcpu_active_end(cpu,other));assert(!armcpu_active_result(cpu).valid);
  ArmCpuActiveClock lease={};other=b;other.leaseGeneration=8;assert(armcpu_active_begin(lease,a));assert(!armcpu_active_end(lease,other));
  ArmCpuActiveClock lost={};other=b;other.leaseHeld=false;assert(armcpu_active_begin(lost,a));assert(!armcpu_active_end(lost,other));
  ArmCpuActiveClock idle={};other=a;other.activeBoundaryKnown=false;assert(!armcpu_active_begin(idle,other));
  ArmCpuActiveClock overflow={};other=b;other.overflow=1ULL<<31;assert(armcpu_active_begin(overflow,a));assert(!armcpu_active_end(overflow,other));
  ArmCpuActiveClock wrapCount={};other=b;other.cycles=1;assert(armcpu_active_begin(wrapCount,a));assert(!armcpu_active_end(wrapCount,other));
  ArmCpuActiveClock backwards={};other=b;other.before=50;other.after=51;assert(armcpu_active_begin(backwards,a));assert(!armcpu_active_end(backwards,other));
  ArmCpuActiveClock timerWrap={};auto w=point(UINT64_MAX-15,UINT64_MAX-13,1000);auto q=point(115,117,11000);assert(armcpu_active_begin(timerWrap,w));assert(armcpu_active_end(timerWrap,q));assert(armcpu_active_result(timerWrap).valid);
  ArmCpuActiveClock changed={};other=b;other.pmcr=3;assert(armcpu_active_begin(changed,a));assert(!armcpu_active_end(changed,other));
  ArmCpuActiveClock sum={};sum.cycles=UINT64_MAX-3;assert(armcpu_active_begin(sum,a));assert(!armcpu_active_end(sum,b));
  ArmCpuActiveClock gap={};assert(armcpu_active_begin(gap,a));assert(armcpu_active_end(gap,b));other=c;other.cycles=1;assert(!armcpu_active_begin(gap,other));
  ArmCpuActiveClock configGap={};assert(armcpu_active_begin(configGap,a));assert(armcpu_active_end(configGap,b));other=c;other.filter=1;assert(!armcpu_active_begin(configGap,other));
  ArmCpuActiveClock minOverflow={};minOverflow.minTicks=UINT64_MAX;assert(armcpu_active_begin(minOverflow,a));assert(!armcpu_active_end(minOverflow,b));
  ArmCpuActiveClock maxOverflow={};maxOverflow.maxTicks=UINT64_MAX;assert(armcpu_active_begin(maxOverflow,a));assert(!armcpu_active_end(maxOverflow,b));
  ArmCpuActiveClock huge={};huge.frequency=1000000000;huge.cycles=UINT64_MAX;huge.minTicks=1;huge.maxTicks=2;huge.intervals=1;assert(!armcpu_active_result(huge).valid);
  ArmCpuActiveClock cpuGap={};assert(armcpu_active_begin(cpuGap,a));assert(armcpu_active_end(cpuGap,b));other=c;other.cpu=1;assert(!armcpu_active_begin(cpuGap,other));assert(!armcpu_active_begin(cpuGap,c));
  ArmCpuActiveClock genGap={};assert(armcpu_active_begin(genGap,a));assert(armcpu_active_end(genGap,b));other=c;other.leaseGeneration=8;assert(!armcpu_active_begin(genGap,other));
  ArmCpuActiveClock freqGap={};assert(armcpu_active_begin(freqGap,a));assert(armcpu_active_end(freqGap,b));other=c;other.frequency=26000000;assert(!armcpu_active_begin(freqGap,other));
  ArmCpuActiveClock reset={};other=a;other.pmcr=3;assert(!armcpu_active_begin(reset,other));
  ArmCpuActiveClock pending={};assert(armcpu_active_begin(pending,a));assert(!armcpu_active_result(pending).valid);assert(!armcpu_active_begin(pending,a));
  puts("PASS active-only interval bounds and fail-closed sample validation");
}
