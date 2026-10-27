#!/usr/bin/env python3
"""Actual ArmCpu checked SYSIRQ/GIC registration, rollback and owned mask."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'ArmCpu.cpp').read_text()
guard=s[s.index('#ifndef ARMCPU_SPI_SERIALIZE'):s.index('static uint64_t gBootAffinity')]
start=s.index('extern "C" IOReturn armcpu_spi_attach_sysirq');end=s.index('\n#endif',start)
body=s[start:end].replace('__asm__ volatile ("dsb sy" ::: "memory");','__atomic_thread_fence (__ATOMIC_SEQ_CST);')
pre=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
using boolean_t=bool;using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBadArgument=2,kIOReturnExclusiveAccess=3,kIOReturnNoResources=4;
#define ARMCPU_SPI_SERIALIZE 1
#define ARMCPU_SPI_SLOTS 16
struct ArmCpuSpi{uint32_t id;void(*handler)(void*);void*context;volatile uint64_t count;};
ArmCpuSpi gSpi[16]{};static volatile uint32_t gSpiUsed=0;thread_local bool irq=true;
bool ml_set_interrupts_enabled(bool v){bool old=irq;irq=v;return old;}
'''
mid=r'''
alignas(8) uint8_t mem[4096]{};uintptr_t gDist=reinterpret_cast<uintptr_t>(mem);bool gGicV2=true;uint32_t armcpu_gic_ready=1;uint64_t gBootAffinity=2;
bool rejectGroup=false,rejectEnable=false;uint32_t lastMask=0;
uint32_t read32(uintptr_t p,unsigned o){return *reinterpret_cast<uint32_t*>(p+o);}
void write32(uintptr_t p,unsigned o,uint32_t v){
 auto reg=reinterpret_cast<uint32_t*>(p+o);
 if(o>=0x180 && o<0x200){lastMask=v;*reinterpret_cast<uint32_t*>(p+o-0x80)&=~v;}
 else if(o>=0x100 && o<0x180){if(!rejectEnable)*reg|=v;}
 else if(o>=0x80 && o<0x100 && rejectGroup){}
 else *reg=v;
}
void handler(void*){}void other(void*){}
'''
post=r'''
int main(){*reinterpret_cast<uint32_t*>(gDist+4)=7;uint32_t pol=0x100,rb=0;uint64_t target=0;bool installed=false;uint32_t bit=1u<<14;
 rejectGroup=true;assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==kIOReturnNotReady);
 assert(!installed && pol==0x100 && lastMask==bit && gSpiUsed==0);
 rejectGroup=false;rejectEnable=true;lastMask=0;assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==kIOReturnNotReady);
 assert(installed && pol==0x100 && lastMask==bit && gSpiUsed==1);
 // Published-but-masked slot remains owned; wrong handler cannot disable it.
 lastMask=0;assert(!armcpu_spi_mask_owned(110,other,nullptr) && !lastMask);
 assert(armcpu_spi_mask_owned(110,handler,nullptr) && lastMask==bit);
 gSpiUsed=0;gSpi[0]={};rejectEnable=false;lastMask=0;pol=0x100;
 assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==0);
 assert(installed && (pol&bit) && (pol&0x100) && rb==0xa003 && target==2);
 uint32_t old=pol;assert(armcpu_spi_attach_sysirq(78,&pol,other,nullptr,&rb,&target,&installed)==kIOReturnExclusiveAccess && pol==old);
 // Different sources sharing the same polarity word retain both updates.
 std::atomic<int> ok{0};auto attach=[&](uint32_t n){bool yes=false;if(!armcpu_spi_attach_sysirq(n,&pol,other,nullptr,nullptr,nullptr,&yes))++ok;};
 std::thread a(attach,76),b(attach,77);a.join();b.join();assert(ok==2 && (pol&(1u<<12)) && (pol&(1u<<13)) && (pol&bit));
 puts("PASS actual SYSIRQ/GIC: RMW synchronization, duplicate refusal, failed group/enable rollback, exact owned mask");}
'''
with tempfile.TemporaryDirectory(prefix='armcpu-sysirq-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+guard+mid+body+post)
 subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
