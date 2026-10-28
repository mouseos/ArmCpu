#!/usr/bin/env python3
"""Exercise the actual attach path with GICv2 Non-secure RAZ/WI models."""
from pathlib import Path
import runpy
import subprocess
import tempfile

base = runpy.run_path(str(Path(__file__).with_name('msdc-sysirq-host.py')))
body = base['body'].replace(
    'uint8_t priority = *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid);',
    'uint8_t priority = rejectPriority ? 0 : *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid);')
mid = base['mid'].replace('bool rejectGroup=false,', 'bool rejectPriority=false;bool rejectGroup=false,')
post = r'''
int main() {
  uint32_t pol=0, rb=0; uint64_t target=0; bool installed=false;
  const uint32_t bit=1u<<14;
  auto reset = [&] { for (auto &v : mem) v=0; pol=rb=0; installed=false;
    gSpiUsed=0; gSpi[0]={}; rejectGroup=true; rejectPriority=false;
    rejectEnable=false; rejectMask=false; lastMask=0; };
  reset(); *reinterpret_cast<uint32_t*>(gDist+4)=7 | (1u<<10);
  auto rc=armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed);
#if ARMCPU_GICV2_ASSUME_NONSECURE
  assert(rc==0 && installed && target==2 && rb==0x1a003 && (read32(gDist,0x10c)&bit));
  reset(); *reinterpret_cast<uint32_t*>(gDist+4)=7 | (1u<<10); rejectPriority=true;
  assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==kIOReturnNotReady);
  assert(!installed && lastMask==bit && pol==0);
  reset(); *reinterpret_cast<uint32_t*>(gDist+4)=7 | (1u<<10); rejectEnable=true;
  assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==kIOReturnNotReady);
  assert(installed && lastMask==bit && pol==0);
  reset(); *reinterpret_cast<uint32_t*>(gDist+4)=7 | (1u<<10); rejectPriority=true; rejectMask=true;
  assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==kIOReturnNotReady);
  assert(installed && armcpu_spi_mask_failed_id==110 && pol==0);
  reset(); *reinterpret_cast<uint32_t*>(gDist+4)=7; // Security Extensions absent: retain group test.
  assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==kIOReturnNotReady);
  assert(!installed && lastMask==bit);
#else
  assert(rc==kIOReturnNotReady && !installed && lastMask==bit);
#endif
  reset(); *reinterpret_cast<uint32_t*>(gDist+4)=7; rejectGroup=false;
  assert(armcpu_spi_attach_sysirq(78,&pol,handler,nullptr,&rb,&target,&installed)==0);
  assert(installed && rb==0xa003);
  puts("PASS GICv2 opt-in group validation and failure rollback");
}
'''
with tempfile.TemporaryDirectory(prefix='armcpu-ns-') as d:
    for enabled in (0, 1):
        src = Path(d) / 'test.cpp'
        exe = Path(d) / 'test'
        src.write_text(base['pre'] + f'\n#define ARMCPU_GICV2_ASSUME_NONSECURE {enabled}\n' +
                       base['guard'] + mid + body + post)
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-pthread', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
