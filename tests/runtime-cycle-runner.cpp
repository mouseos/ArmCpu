#include "../CycleClock.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#define ARMCPU_RUNTIME_TEST 1
using wait_result_t = int;
struct CPU { int state; };
using processor_t = CPU *;
enum { PROCESSOR_IDLE=4, PROCESSOR_DISPATCHING=5, PROCESSOR_RUNNING=6 };
static CPU cpus[4] = {{6},{6},{6},{6}};
static processor_t current = cpus, bound = nullptr;
static bool irq = true, preempt = true, running = false;
static unsigned samples = 0, logs = 0, terminations = 0, bindCount = 0;
static bool owner=true, hotplugAvailable=true, hotplugLocked=false, lateBind=false, watchdogArmed=true, interference=false;
static bool a04br3_runtime_recovery_recheck(){return watchdogArmed;}
static volatile uint32_t a04br3_runtime_recovery_ready=1, a04br3_runtime_probe_active=0;
static uint64_t uptime=0;
static volatile uint32_t armcpu_quiesce_calls=0, armcpu_halt_calls=0, armcpu_smp_step[8]={0,7,7,7};
static void clock_interval_to_deadline(unsigned, unsigned, uint64_t *d){*d=uptime+250;}
static void clock_get_uptime(uint64_t *t){*t=uptime;}
static uint64_t now = 0, counter = 77, control = 8;
static processor_t cpu_to_processor(int i){return i < 4 ? &cpus[i] : nullptr;}
static processor_t current_processor(){return current;}
static bool armcpu_runtime_policy_allows(unsigned){return owner;}
static bool armcpu_runtime_online(processor_t p, unsigned cpu){return p==&cpus[cpu] && p->state>=PROCESSOR_IDLE && p->state<=PROCESSOR_RUNNING;}
static bool armcpu_runtime_try_hotplug_lock(){if(!hotplugAvailable)return false; assert(!hotplugLocked);hotplugLocked=true;return true;}
static void armcpu_runtime_hotplug_unlock(){assert(hotplugLocked);hotplugLocked=false;}
static processor_t thread_bind(processor_t p){auto prev=bound;bound=p;++bindCount;return prev;}
#define THREAD_CONTINUE_NULL nullptr
static void thread_block(decltype(nullptr)){assert(irq && preempt && bound && hotplugLocked && a04br3_runtime_probe_active);current=bound;if(lateBind)uptime+=300;}
static bool ml_get_interrupts_enabled(){return irq;}
static bool ml_set_interrupts_enabled(bool on){bool old=irq;irq=on;return old;}
static void disable_preemption(){assert(preempt);preempt=false;}
static void enable_preemption(){assert(!preempt);preempt=true;}
static void IOSleep(unsigned){assert(irq && preempt);}
static void IOLog(const char *, ...){assert(irq && preempt);++logs;}
static int current_thread(){return 1;}
static void thread_terminate(int){++terminations;}
struct ArmCpuCycleRegisters {
  unsigned source=0;
  uint64_t mpidr(){assert(irq&&!preempt);return static_cast<uint64_t>(current-cpus);}
  uint64_t counter(unsigned){assert(irq&&!preempt);now+=100; if(running) counterValueIncrement(); return now;}
  void counterValueIncrement(){::counter+=10000;}
  uint64_t id(){return 1ULL<<8;} uint64_t frequency(){return 26000000;}
  uint64_t pmcr(){return control;} uint64_t enabled(){return running ? ((1ULL << 31) | (interference ? 1ULL : 0ULL)) : 0;} uint64_t filter(){return 0;}
  uint64_t interrupts(){return 0;} uint64_t overflow(){return 0;}
  uint64_t cycle(){return ::counter;}
  uint64_t time(){return this->counter(source);}
  void clear_overflow(uint64_t){assert(false);}
  void set_cycle(uint64_t x){assert(irq&&!preempt);::counter=x;}
  void set_pmcr(uint64_t x){assert(irq&&!preempt);control=x;}
  void enable_cycle(){assert(irq&&!preempt && a04br3_runtime_probe_active);running=true;}
  void disable_cycle(){assert(irq&&!preempt);running=false;++samples;}
  void barrier(){}
};
#include "../RuntimeCycleRunner.h"
int main(){
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==12 && logs==48 && terminations==1 && bindCount==24);
  assert(irq && preempt && !running && counter==77 && control==8 && bound==nullptr);
  cpus[2].state=0; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==9 && logs==39 && terminations==1 && bindCount==18);
  assert(irq && preempt && !running && counter==77 && control==8);
  cpus[2].state=6; control=1; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==48 && terminations==1 && bindCount==24);
  assert(irq && preempt && !running && counter==77 && control==1);
  owner=false; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==12 && terminations==1 && bindCount==0 && !hotplugLocked);
  owner=true; hotplugAvailable=false; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==12 && terminations==1 && bindCount==0 && !hotplugLocked);
  hotplugAvailable=true; lateBind=true; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==12 && terminations==1 && bindCount==24 && !hotplugLocked);
  lateBind=false; watchdogArmed=false; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==12 && terminations==1 && bindCount==24 && !hotplugLocked && !a04br3_runtime_probe_active);
  watchdogArmed=true; control=8; counter=77; interference=true; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && terminations==1 && bindCount==2 && !hotplugLocked && a04br3_runtime_probe_active);
  assert(running && control==1); // observed conflict: do not overwrite the other owner's state
  running=false; control=8; counter=77; interference=false; a04br3_runtime_probe_active=0;
  armcpu_quiesce_calls=1; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==12 && terminations==1 && bindCount==0 && !hotplugLocked);
  armcpu_quiesce_calls=0; a04br3_runtime_recovery_ready=0; samples=logs=terminations=bindCount=0;
  armcpu_runtime_cycle_worker(nullptr,0);
  assert(samples==0 && logs==12 && terminations==1 && bindCount==0 && !a04br3_runtime_probe_active);
  puts("PASS: CPU binding, IRQ-safe logs, PMU restoration and offline refusal");
}
