#!/usr/bin/env python3
"""Run the actual ArmCpu interrupt dispatch against a mocked GICv2 IAR/EOIR."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'ArmCpu.cpp').read_text()
start=s.index('void ArmCpu::interrupt (void)');end=s.index('\nvoid ArmCpu::signalCPU',start)
body=s[start:end]
body=body.replace('__asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (id));','id = 1023;')
body=body.replace('__asm__ volatile ("dsb ish" ::: "memory");','')
body=body.replace('__asm__ volatile ("isb" ::: "memory");','')
body=body.replace('__asm__ volatile ("msr icc_eoir1_el1, %0\\n\\tisb" :: "r" (id) : "memory");','')
pre=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
struct ArmCpuSpi{uint32_t id;void(*handler)(void*);void*context;volatile uint64_t count;};
ArmCpuSpi gSpi[16]{};volatile uint32_t gSpiUsed=0;uintptr_t gCpuIf=1,gDist=2;
bool gGicV2=true,gSpiIpi=false;constexpr uint32_t kGicV2IpiSgi=1,kIpiSpiBase=1008;
uint32_t armcpu_irq_phase=0,armcpu_irq_id=0,armcpu_core_timer[8]{},armcpu_core_sgi[8]{},armcpu_spi_unhandled_id=0;
uint64_t armcpu_timer_interrupts=0,armcpu_ipi_interrupts=0,armcpu_spi_unhandled=0;
void(*ipi_handler)()=nullptr;void rtclock_intr(int){}void panic(const char*,...){abort();}
uint32_t iar=1023,lastEoi=0,disabled=0,handlerCalled=0;
uint32_t read32(uintptr_t base,unsigned off){assert(base==gCpuIf && off==12);uint32_t n=iar;iar=1023;return n;}
void write32(uintptr_t base,unsigned off,uint32_t value){if(base==gCpuIf){assert(off==16 && handlerCalled);lastEoi=value;}else{assert(base==gDist && off==0x18c);disabled=value;handlerCalled=1;}}
void handler(void*){assert(lastEoi==0);++handlerCalled;}
struct ArmCpu{bool diagnosePower=false;unsigned logicalId=0;void interrupt();};
'''
post=r'''
int main(){ArmCpu cpu;gSpi[0]={110,handler,nullptr,0};gSpiUsed=1;iar=110;cpu.interrupt();
 assert(handlerCalled==1 && lastEoi==110 && gSpi[0].count==1 && armcpu_spi_unhandled==0);
 handlerCalled=0;lastEoi=0;iar=111;cpu.interrupt();assert(disabled==(1u<<15) && lastEoi==111 && armcpu_spi_unhandled_id==111);
 puts("PASS actual GICv2 interrupt dispatch: handler before EOI, raw IAR EOI, unhandled SPI masked");}
'''
with tempfile.TemporaryDirectory(prefix='armcpu-dispatch-') as d:
 p=Path(d)/'a.cpp';x=Path(d)/'a';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(x)],check=True)
 subprocess.run([str(x)],check=True)
