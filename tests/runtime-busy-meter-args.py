#!/usr/bin/env python3
"""Exercise actual arw=4 meter refusal with a host boot-argument stub."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'ArmCpu.cpp').read_text()
a=s.index('  unsigned cpuMeter = 0;')
b=s.index('  } else if (gGicV2',a)
body=s[a:b].replace('if (ml_get_interrupts_enabled ()) IOLog ("ArmCpu: runtime-window error=cpu-meter-conflict\\n");','return true;')
pre='''#include <cassert>\n#include <map>\n#include <string>\nstd::map<std::string,unsigned> args;\nbool PE_parse_boot_argn(const char *n,unsigned *p,unsigned){auto it=args.find(n);if(it==args.end())return false;*p=it->second;return true;}\n#define ARMCPU_RUNTIME_BUSY_CONTROL 1\n#define ARMCPU_RUNTIME_ACTIVE 1\nbool conflict(bool windowSpecified,unsigned runtimeWindow){\n'''
post='''  }\n  return false;\n}\nint main(){assert(!conflict(true,4));args["cm"]=1;assert(conflict(true,4));args["cm"]=3;assert(conflict(true,4));assert(!conflict(true,3));assert(!conflict(false,4));args["cm"]=0;assert(!conflict(true,4));}\n'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp'; exe=Path(d)/'test';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: arw=4 refuses configured CPU meter; other modes unchanged')
