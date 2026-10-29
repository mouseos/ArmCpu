#include "../StartupCycleGuard.h"
#include <cassert>
struct Board {
 bool available=true,recovery=true,active=false; unsigned beginCalls=0,endCalls=0,busyCalls=0;
 bool begin(){++beginCalls;if(!available||active||beginCalls<=busyCalls)return false;active=true;return true;}
 bool recoveryReady(){return recovery;}
 void end(){assert(active);++endCalls;active=false;}
};
struct Registers {
 uint64_t clock=0,value=123,control=8;bool enabledNow=false,conflict=false,stuck=false,backwards=false;unsigned writes=0;
 uint64_t id(){return 1ULL<<8;} uint64_t frequency(){return 13000000;}
 uint64_t pmcr(){return control;} uint64_t enabled(){return enabledNow?((1ULL<<31)|(conflict?1:0)):0;}
 uint64_t filter(){return 0;} uint64_t interrupts(){return 0;} uint64_t overflow(){return 0;}
 uint64_t cycle(){return value;} uint64_t time(){if(backwards)--clock;else if(!stuck)clock+=10;if(enabledNow)value+=1000;return clock;}
 void set_cycle(uint64_t x){value=x;++writes;} void set_pmcr(uint64_t x){control=x;++writes;}
 void enable_cycle(){enabledNow=true;++writes;} void disable_cycle(){enabledNow=false;++writes;}
 void clear_overflow(uint64_t){assert(false);} void barrier(){}
};
int main(){
 Board b; Registers r; auto s=armcpu_startup_guarded_sample(b,r);
 assert(s.status==0&&!b.active&&b.endCalls==1&&!r.enabledNow&&r.value==123&&r.control==8);
 Board delayed;delayed.busyCalls=20;Registers delayedRegs;s=armcpu_startup_guarded_sample(delayed,delayedRegs);
 assert(s.status==0&&delayed.beginCalls==21&&!delayed.active&&delayed.endCalls==1);
 Board busy;busy.available=false;Registers a;s=armcpu_startup_guarded_sample(busy,a);
 assert(s.status==12&&!a.writes&&!busy.active&&!busy.endCalls);
 Board stalled;stalled.available=false;Registers t;t.stuck=true;s=armcpu_startup_guarded_sample(stalled,t);
 assert(s.status==12&&!t.writes&&stalled.beginCalls==1000001);
 Board reverse;reverse.available=false;Registers z;z.backwards=true;s=armcpu_startup_guarded_sample(reverse,z);
 assert(s.status==12&&!z.writes&&reverse.beginCalls==1);
 Board bad;bad.recovery=false;Registers c;s=armcpu_startup_guarded_sample(bad,c);
 assert(s.status==13&&!c.writes&&!bad.active&&bad.endCalls==1);
 Board clash;Registers x;x.conflict=true;s=armcpu_startup_guarded_sample(clash,x);
 assert(s.status==11&&clash.active&&!clash.endCalls&&x.enabledNow&&x.control==1);
 Registers next;s=armcpu_startup_guarded_sample(clash,next);
 assert(s.status==12&&!next.writes&&clash.active);
}
