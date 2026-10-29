#include "../CycleBounds.h"
#include <cassert>
#include <cstdint>
int main() {
  auto b=armcpu_cycle_bounds(1000,100,10,12,22,25);
  assert(b.valid && b.minTicks==10 && b.maxTicks==15 && b.lowerHz==6666 && b.upperHz==10000);
  b=armcpu_cycle_bounds(1,10,UINT64_MAX-2,UINT64_MAX,4,5);
  assert(b.valid && b.minTicks==5 && b.maxTicks==8 && b.lowerHz==1 && b.upperHz==2);
  assert(!armcpu_cycle_bounds(0,10,1,2,3,4).valid);
  assert(!armcpu_cycle_bounds(1,0,1,2,3,4).valid);
  assert(!armcpu_cycle_bounds(1,10,1,2,2,4).valid);
  assert(!armcpu_cycle_bounds(1,10,2,1,3,4).valid);
  assert(!armcpu_cycle_bounds(UINT64_MAX,UINT64_MAX,0,0,1,1).valid);
  b=armcpu_cycle_bounds(1,10,1,1,4,4);
  assert(b.valid && b.lowerHz==3 && b.upperHz==4);
}
