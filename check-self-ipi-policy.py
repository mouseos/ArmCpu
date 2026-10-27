#!/usr/bin/env python3
"""Exercise the actual boot policy block for both controller types and argument values."""
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).parent/'ArmCpu.cpp').read_text();start=s[s.index('bool ArmCpu::start ('):s.index('void ArmCpu::drainPending')]
secondary=start[start.index('  if (secondary) {'):start.index('  } else {')]
assert not any(x in secondary for x in ('gSelfIpi =','gSpiIpi =','diagnosePower ='))
a=start.index('    unsigned selfIpi = 0;');b=start.index('    uint64_t bootMpidr;',a);policy=start[a:b]
a=start.index('    if (gGicV2) {',b);b=start.index('\n    }',a)+6;v2=start[a:b]
h=r'''#include <cassert>
#include <cstring>
static bool gSelfIpi,gSpiIpi,diagnosePower,gGicV2;
static bool givenSelf,givenSpi;static unsigned selfArg,spiArg;
static bool PE_parse_boot_argn(const char *name,void *out,unsigned size){
 assert(size==sizeof(unsigned));if(!strcmp(name,"armcpu-self-ipi")){if(givenSelf)*(unsigned*)out=selfArg;return givenSelf;}
 if(!strcmp(name,"armcpu-spi-ipi")){if(givenSpi)*(unsigned*)out=spiArg;return givenSpi;}return false;
}
static void boot(){
'''+policy+v2+r'''
}
int main(){
 gGicV2=true;boot();assert(gSelfIpi&&!gSpiIpi);
 givenSelf=true;selfArg=0;boot();assert(!gSelfIpi);
 selfArg=1;givenSpi=true;spiArg=1;boot();assert(gSelfIpi&&!gSpiIpi);
 gGicV2=false;givenSelf=false;boot();assert(!gSelfIpi&&gSpiIpi);
 givenSelf=true;selfArg=1;boot();assert(gSelfIpi&&gSpiIpi);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'check.cpp').write_text(h);subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p/'check.cpp'),'-o',str(p/'check')],check=True);subprocess.run([str(p/'check')],check=True)
print('PASS actual boot-only IPI policy: GICv2/v3 defaults, explicit enable/disable, SPI restriction; secondary never changes policy')
