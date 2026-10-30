#!/usr/bin/env python3
"""Verify retained, default-off observer reference with Darwin ARM64 tools."""
from pathlib import Path
import os, shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
clang=os.environ.get('CLANG',shutil.which('clang-18') or shutil.which('clang'))
nm=os.environ.get('LLVM_NM',shutil.which('llvm-nm-18') or shutil.which('llvm-nm'))
objdump=os.environ.get('LLVM_OBJDUMP',shutil.which('llvm-objdump-18') or shutil.which('llvm-objdump'))
ld=os.environ.get('LD64_LLD',shutil.which('ld64.lld-18') or shutil.which('ld64.lld'))
if not all((clang,nm,objdump,ld)):raise SystemExit('Darwin ARM64 clang, llvm-nm, llvm-objdump and ld64.lld required')
source=(root/'ArmCpu.cpp').read_text();header=(root/'SwitchObserverAbiProbe.h').read_text()
assert source.count('#include "SwitchObserverAbiProbe.h"')==1
assert 'armcpu_switch_observer_abi_probe' not in source
assert header.count('armcpu_switch_observer_abi_probe')==1
with tempfile.TemporaryDirectory() as d:
 d=Path(d);unit=d/'probe.cpp';provider=d/'provider.cpp';unit.write_text('#include "SwitchObserverAbiProbe.h"\n')
 provider.write_text('extern "C" bool unixnu_current_thread_switches(unsigned *p) { if (!p) return false; *p=0; return true; }\n')
 common=[clang,'-target','arm64-apple-ios17.0','-ffreestanding','-O2','-I',str(root)]
 for mode in ('0','1'):
  obj=d/('probe'+mode+'.o');subprocess.run(common+['-DARMCPU_SWITCH_ABI_PROBE='+mode,'-c',str(unit),'-o',str(obj)],check=True)
  names=subprocess.check_output([nm,str(obj)],text=True)
  if mode=='0':assert 'armcpu_switch_observer_abi_probe' not in names and 'unixnu_current_thread_switches' not in names
  else:
   assert 'armcpu_switch_observer_abi_probe' in names and ' U _unixnu_current_thread_switches' in names
   relocs=subprocess.check_output([objdump,'--macho','--reloc',str(obj)],text=True)
   assert 'BR26' in relocs and '_unixnu_current_thread_switches' in relocs
 subprocess.run(common+['-c',str(provider),'-o',str(d/'provider.o')],check=True)
 linked=d/'linked.dylib';subprocess.run([ld,'-arch','arm64','-platform_version','ios','17.0','17.0','-dylib','-dead_strip','-o',str(linked),str(d/'probe1.o'),str(d/'provider.o')],check=True)
 names=subprocess.check_output([nm,str(linked)],text=True)
 assert 'armcpu_switch_observer_abi_probe' in names and 'unixnu_current_thread_switches' in names
 dis=subprocess.check_output([objdump,'--macho','--disassemble',str(linked)],text=True)
 assert '\tbl\t_unixnu_current_thread_switches' in dis
print('PASS default-off and retained opt-in observer call in ARM64 object and dead-stripped test link')
