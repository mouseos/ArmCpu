#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'ArmCpu.cpp').read_text(); a=s.index('  unsigned cycleClock = 0;');b=s.index('  uint32_t physical;',a);body=s[a:b].replace('  diagnoseCycleClock = cycleClock == 1;', '  return cycleClock == 1;')
pre='''#include <cassert>\n#include <map>\n#include <string>\nstd::map<std::string,unsigned> args;\nbool PE_parse_boot_argn(const char *n,unsigned *p,unsigned){auto it=args.find(n);if(it==args.end())return false;*p=it->second;return true;}\nbool enabled(){\n'''
post='''}\nint main(){assert(!enabled());args["cc"]=1;assert(enabled());args["armcpu-cycle-clock"]=0;assert(!enabled());args["armcpu-cycle-clock"]=1;args["cc"]=0;assert(enabled());args["armcpu-cycle-clock"]=2;assert(!enabled());}\n'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'x.cpp';x=Path(d)/'x';p.write_text(pre+body+post);subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p),'-o',str(x)],check=True);subprocess.run([str(x)],check=True)
assert 'armcpu_startup_guarded_sample(board, registers)' in s
print('PASS secondary startup default off, cc alias, long-form precedence and interference check')
