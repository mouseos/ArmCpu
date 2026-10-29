#include "../CycleClock.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#define ARMCPU_RUNTIME_TEST 1
#define ARMCPU_RUNTIME_REPEAT_ROUNDS 3U
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
static bool watchdogGateAvailable=true; static unsigned gateBegins=0;
static bool a04br3_runtime_probe_begin(){++gateBegins;if(!watchdogGateAvailable || a04br3_runtime_probe_active)return false;a04br3_runtime_probe_active=1;return true;}
static void a04br3_runtime_probe_end(){a04br3_runtime_probe_active=0;}
static uint64_t uptime=0;
static volatile uint32_t armcpu_quiesce_calls=0, armcpu_halt_calls=0, armcpu_smp_step[8]={0,7,7,7};
static void clock_interval_to_deadline(unsigned, unsigned, uint64_t *d){*d=uptime+250;}
static void clock_get_uptime(uint64_t *t){*t=uptime;}
static uint64_t now = 0, counter = 77, control = 8; static unsigned pmuReads=0;
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
static uint64_t testEpoch=0; static bool triggerIdle=false, failRecoveryDuringSleep=false, longSleep=false, checkRepeatSleep=false;
static bool leaseActive=false; static unsigned repeatWaits=0, windowWaits=0;
static void IOSleep(unsigned ms){assert(irq && preempt);if(ms==5000 && checkRepeatSleep){assert(!running&&!leaseActive&&!bound&&!a04br3_runtime_probe_active&&!hotplugLocked && control==8 && counter==77);++repeatWaits;}if((ms==2 || ms==10) && running){if(checkRepeatSleep){assert(ms==10);++windowWaits;}now+=longSleep?1400000:13000*ms;counter+=2000000;if(triggerIdle)testEpoch+=2;if(failRecoveryDuringSleep)watchdogArmed=false;}}
static unsigned loggedStatus=999, loggedCpu=999;static unsigned long long loggedMpidr=0;static char loggedLine[1024];
static void IOLog(const char *format, ...){assert(irq && preempt);++logs;va_list ap;va_start(ap,format);vsnprintf(loggedLine,sizeof(loggedLine),format,ap);va_end(ap);unsigned cpu,status;unsigned long long mpidr;if(sscanf(loggedLine,"ArmCpu: runtime-window cpu=%u mpidr=%llx status=%u",&cpu,&mpidr,&status)==3){loggedCpu=cpu;loggedMpidr=mpidr;loggedStatus=status;}}
static int current_thread(){return 1;}
static void thread_terminate(int){++terminations;}
struct ArmCpuCycleRegisters {
  unsigned source=0;
  uint64_t mpidr(){assert(irq&&!preempt);return static_cast<uint64_t>(current-cpus);}
  uint64_t counter(unsigned){assert(irq&&!preempt);now+=100; if(running) counterValueIncrement(); return now;}
  void counterValueIncrement(){::counter+=10000;}
  uint64_t id(){++pmuReads;return 1ULL<<8;} uint64_t frequency(){return 26000000;}
  uint64_t pmcr(){++pmuReads;return control;} uint64_t enabled(){++pmuReads;return running ? ((1ULL << 31) | (interference ? 1ULL : 0ULL)) : 0;} uint64_t filter(){++pmuReads;return 0;}
  uint64_t interrupts(){++pmuReads;return 0;} uint64_t overflow(){++pmuReads;return 0;}
  uint64_t cycle(){++pmuReads;return ::counter;}
  uint64_t time(){return this->counter(source);}
  void clear_overflow(uint64_t){assert(false);}
  void set_cycle(uint64_t x){assert(irq&&!preempt);::counter=x;}
  void set_pmcr(uint64_t x){assert(irq&&!preempt);control=x;}
  void enable_cycle(){assert(irq&&!preempt && a04br3_runtime_probe_active);running=true;}
  void disable_cycle(){assert(irq&&!preempt);running=false;++samples;}
  void barrier(){}
};
static bool leaseQualified=true, gateReleaseFailure=false, leaseReleaseFailure=false, failLaterRelease=false;static uint64_t leaseToken=0;
static bool revokeAfterPreflight=false;
static bool a04br3_pmu_qualification_available(unsigned){bool result=leaseQualified;if(revokeAfterPreflight)leaseQualified=false;return result;}
static bool a04br3_pmu_lease_acquire(unsigned cpu,uint64_t *token){assert(a04br3_runtime_probe_active);if(!leaseQualified || leaseActive)return false;leaseActive=true;leaseToken=cpu+1;*token=leaseToken;return true;}
static bool a04br3_pmu_lease_valid(unsigned cpu,uint64_t token){return leaseActive&&token==leaseToken&&token==cpu+1;}
static bool a04br3_pmu_lease_release(unsigned cpu,uint64_t token){if(leaseReleaseFailure||(failLaterRelease && samples>=5)||!a04br3_pmu_lease_valid(cpu,token))return false;leaseActive=false;return true;}
static bool a04br3_runtime_probe_end_checked(){if(gateReleaseFailure||leaseActive||!a04br3_runtime_probe_active)return false;a04br3_runtime_probe_end();return true;}
static bool unixnu_idle_epoch_observe(int *actual,uint64_t *epoch,unsigned *reason,uint64_t *old,uint64_t *seen,unsigned *entry){*actual=(int)(current-cpus);*epoch=testEpoch;*reason=0;*old=0;*seen=0;*entry=0;return true;}
static uint64_t unixnu_idle_epoch_current(unsigned cpu){assert(!preempt);return current==&cpus[cpu]?testEpoch:UINT64_MAX;}
#if ARMCPU_RUNTIME_ACTIVE
static bool fakeActiveStart=true, fakeActiveStop=true;
static bool unixnu_pmuv3_active_start(unsigned cpu, uint64_t gen){assert(!irq && !preempt && current==&cpus[cpu] && gen);return fakeActiveStart;}
static bool unixnu_pmuv3_active_stop(unsigned cpu,uint64_t gen,uint64_t *cy,uint64_t *mi,uint64_t *ma,unsigned *n){assert(!irq && !preempt && current==&cpus[cpu] && gen);*cy=2000000;*mi=26000;*ma=26200;*n=triggerIdle?2:1;return fakeActiveStop;}
#endif
#include "../RuntimeCycleRunner.h"
#include "../RuntimeWindowRunner.h"
int main(){
 armcpu_runtime_window_worker(nullptr,0);assert(samples==4&&!running&&!leaseActive&&!armcpu_window_poison&&!a04br3_runtime_probe_active&&bound==nullptr&&control==8&&counter==77&&terminations==1&&loggedCpu==3&&loggedStatus==0);
 samples=logs=terminations=bindCount=gateBegins=pmuReads=0;leaseQualified=false;armcpu_runtime_window_worker(nullptr,0);assert(samples==0&&bindCount==0&&gateBegins==0&&pmuReads==0&&!leaseActive&&!armcpu_window_poison&&!a04br3_runtime_probe_active&&terminations==1);
 leaseQualified=true;samples=logs=terminations=bindCount=0;triggerIdle=true;armcpu_runtime_window_worker(nullptr,0);assert(samples==4&&!running&&!armcpu_window_poison&&!a04br3_runtime_probe_active&&!leaseActive&&terminations==1&&loggedStatus==(ARMCPU_RUNTIME_ACTIVE?0U:13U));
#if ARMCPU_RUNTIME_ACTIVE
 triggerIdle=false;fakeActiveStart=false;samples=logs=terminations=bindCount=0;armcpu_runtime_window_worker(nullptr,0);assert(samples==0&&loggedStatus==11&&armcpu_window_poison&&a04br3_runtime_probe_active);armcpu_window_poison=0;a04br3_runtime_probe_active=0;leaseActive=false;running=false;control=8;counter=77;fakeActiveStart=true;
 fakeActiveStop=false;samples=logs=terminations=bindCount=0;armcpu_runtime_window_worker(nullptr,0);assert(samples==0&&loggedStatus==11&&armcpu_window_poison&&a04br3_runtime_probe_active);armcpu_window_poison=0;a04br3_runtime_probe_active=0;leaseActive=false;running=false;control=8;counter=77;fakeActiveStop=true;
#endif
 triggerIdle=false;longSleep=true;samples=logs=terminations=bindCount=0;armcpu_runtime_window_worker(nullptr,0);assert(samples==4&&!armcpu_window_poison&&loggedStatus==16);longSleep=false;
 lateBind=true;samples=logs=terminations=bindCount=0;armcpu_runtime_window_worker(nullptr,0);assert(samples==0&&!armcpu_window_poison&&!a04br3_runtime_probe_active);lateBind=false;
 leaseReleaseFailure=true;samples=logs=terminations=bindCount=0;armcpu_runtime_window_worker(nullptr,0);assert(samples==1&&armcpu_window_poison&&a04br3_runtime_probe_active&&leaseActive&&loggedStatus==15);leaseReleaseFailure=false;armcpu_window_poison=0;a04br3_runtime_probe_active=0;leaseActive=false;
 failRecoveryDuringSleep=true;samples=logs=terminations=bindCount=0;armcpu_runtime_window_worker(nullptr,0);assert(samples==1&&!running&&armcpu_window_poison&&a04br3_runtime_probe_active&&leaseActive&&loggedStatus==15);failRecoveryDuringSleep=false;watchdogArmed=true;armcpu_window_poison=0;a04br3_runtime_probe_active=0;leaseActive=false;
 samples=logs=terminations=bindCount=gateBegins=pmuReads=0;leaseQualified=true;revokeAfterPreflight=true;armcpu_runtime_window_worker(nullptr,0);assert(bindCount==2&&gateBegins==1&&pmuReads==0&&!armcpu_window_poison&&!a04br3_runtime_probe_active&&!leaseActive);revokeAfterPreflight=false;
 samples=logs=terminations=bindCount=0;leaseQualified=true;revokeAfterPreflight=true;gateReleaseFailure=true;armcpu_runtime_window_worker(nullptr,0);assert(armcpu_window_poison&&a04br3_runtime_probe_active&&bindCount==2);gateReleaseFailure=false;leaseQualified=true;revokeAfterPreflight=false;armcpu_window_poison=0;a04br3_runtime_probe_active=0;
 samples=logs=terminations=bindCount=0;interference=true;armcpu_runtime_window_worker(nullptr,0);assert(samples==0&&running&&armcpu_window_poison&&a04br3_runtime_probe_active&&leaseActive&&terminations==1&&loggedStatus==11);
 unsigned previousBinds=bindCount;armcpu_runtime_window_worker(nullptr,0);assert(bindCount==previousBinds&&armcpu_window_poison&&a04br3_runtime_probe_active);
 interference=false;armcpu_window_poison=0;a04br3_runtime_probe_active=0;leaseActive=false;running=false;control=8;counter=77;
 samples=logs=terminations=bindCount=0;checkRepeatSleep=true;armcpu_runtime_window_worker(reinterpret_cast<void *>(1),0);assert(samples==12&&!running&&!leaseActive&&!armcpu_window_poison&&!a04br3_runtime_probe_active&&!hotplugLocked&&terminations==1&&repeatWaits==3&&windowWaits==12);
 samples=logs=terminations=bindCount=0;failLaterRelease=true;armcpu_runtime_window_worker(reinterpret_cast<void *>(1),0);assert(samples==5&&armcpu_window_poison&&a04br3_runtime_probe_active&&leaseActive&&terminations==1);
 puts("PASS runtime window worker: CPU0-3, scheduler interval, fail-closed lease, idle and watchdog quarantine");
}
