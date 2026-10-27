#!/usr/bin/env python3
"""Exercise ArmCpu's real registration code on a modeled GICv2 register array."""
from pathlib import Path
import subprocess, tempfile
s=(Path(__file__).resolve().parents[1]/'ArmCpu.cpp').read_text()
guard=s[s.index('#ifndef ARMCPU_SPI_SERIALIZE'):s.index('static uint64_t gBootAffinity')]
body=s[s.index('extern "C" IOReturn armcpu_spi_attach'):s.index('\nextern "C" uint64_t armcpu_spi_count')]
body=body.replace('__asm__ volatile ("dsb sy" ::: "memory");','__atomic_thread_fence (__ATOMIC_SEQ_CST);')
pre=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
using boolean_t=bool; using IOReturn=int;
constexpr int kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnBadArgument=2,kIOReturnExclusiveAccess=3,kIOReturnNoResources=4,kIOReturnTimeout=5;
#define ARMCPU_SPI_SERIALIZE 1
#define ARMCPU_SPI_SLOTS 16
struct ArmCpuSpi {uint32_t id; void (*handler)(void*);void *context;volatile uint64_t count;};
ArmCpuSpi gSpi[16];static volatile uint32_t gSpiUsed=0;
thread_local bool irq=true;
bool ml_set_interrupts_enabled(bool b){bool old=irq;irq=b;return old;}
'''
mid=r'''
uint64_t gBootAffinity=2; alignas(8) uint8_t memory[0x1000]{};
uintptr_t gDist=reinterpret_cast<uintptr_t>(memory); uint32_t armcpu_gic_ready=1; bool gGicV2=true;
uint32_t read32(uintptr_t p,unsigned o){return *reinterpret_cast<uint32_t*>(p+o);}
void write32(uintptr_t p,unsigned o,uint32_t v){*reinterpret_cast<uint32_t*>(p+o)=v;}
bool waitClear(uintptr_t,unsigned,uint32_t){return true;}
void handler(void*){}
'''
post=r'''
int main(){*reinterpret_cast<uint32_t*>(gDist+4)=7;uint32_t rb=0;uint64_t target=0;
 assert(armcpu_spi_attach(110,false,handler,nullptr,&rb,&target)==0);
 assert((rb&7)==3 && target==2 && gSpiUsed==1 && gSpi[0].id==110);
 assert(armcpu_spi_attach(110,false,handler,nullptr,nullptr,nullptr)==kIOReturnExclusiveAccess);
 assert(armcpu_spi_attach(31,false,handler,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
 std::atomic<int> ok{0},duplicate{0};std::vector<std::thread> threads;
 for(int i=0;i<8;i++)threads.emplace_back([&]{int r=armcpu_spi_attach(120,false,handler,nullptr,nullptr,nullptr);if(r==0)++ok;else if(r==kIOReturnExclusiveAccess)++duplicate;assert(irq);});
 for(auto &t:threads){t.join();} assert(ok==1 && duplicate==7 && gSpiUsed==2);
 assert(memory[0x800+110]==2);puts("PASS actual GICv2 SPI attach: level, group, target, duplicates and concurrent registration");}
'''
with tempfile.TemporaryDirectory(prefix='armcpu-spi-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+guard+mid+body+post)
 subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-pthread',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
