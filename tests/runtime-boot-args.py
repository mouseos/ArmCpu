#!/usr/bin/env python3
"""Compile the actual argument selection with a host PE parser stub."""
from pathlib import Path
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'ArmCpu.cpp').read_text()
a=s.index('  unsigned runtimeCycle = 0;',s.index('#if ARMCPU_RUNTIME_A04BR3',s.index('registerService ();')))
b=s.index(' {\n    auto runtimeRoot',a)
body=s[a:b].replace('  if (gGicV2 && runtimeSpecified && runtimeCycle == 1)', '  return gGicV2 && runtimeSpecified && runtimeCycle == 1;')
pre='''#include <cassert>\n#include <cstring>\n#include <map>\n#include <string>\nstd::map<std::string,unsigned> args; bool gGicV2=true;\nbool PE_parse_boot_argn(const char *name, unsigned *p, unsigned) { auto it=args.find(name); if(it==args.end())return false; *p=it->second; return true; }\nbool enabled() {\n'''
post='''}\nint main(){assert(!enabled());args["arc"]=1;assert(enabled());args["armcpu-runtime-cycle"]=0;assert(!enabled());args["armcpu-runtime-cycle"]=1;gGicV2=false;assert(!enabled());gGicV2=true;assert(enabled());}\n'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.cpp'; exe=Path(d)/'test';p.write_text(pre+body+post)
 subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: runtime cycle default, short form, long-form precedence and GIC guard')
