#!/usr/bin/env python3
"""Check an actual ArmCpu.o using REA; never substitute host-model results."""
import json
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit('usage: python3 tests/check-arm64-rea.py /absolute/path/ArmCpu.o')
obj = sys.argv[1]

def run(*args):
    return json.loads(subprocess.check_output(['rea', *args, '--provider', 'ghidra', '--format', 'json'], text=True))

search = run('search', obj, 'ArmCpu::initCPU', '--kind', 'procedures')
hits = [x for x in search['normalized_result'] if x['value'] == 'ArmCpu::initCPU']
if not hits:
    raise SystemExit('REA did not find initCPU')
entry = min(hits, key=lambda h: int(h['address'], 16))['address']
result = run('instructions', obj, entry)
ins = result['normalized_result']['instructions']
for reg in ('cntvct_el0', 'cntpct_el0', 'pmccntr_el0'):
    matches = [i for i, line in enumerate(ins) if ': mrs ' in line.lower() and reg in line.lower()]
    if not matches or not any(i and ': isb' in ins[i-1].lower() for i in matches):
        raise SystemExit('missing ISB/MRS pair for ' + reg)
print('PASS: initCPU includes ISB/MRS for CNTVCT, CNTPCT and PMCCNTR')
print('REA evidence:', result['evidence_id'])

# Check the GICv2 wake timebase and active signal path, as well as the
# virtual timer enable sequence. These operate on the compiled object.
def procedure(name):
    found = run('search', obj, name, '--kind', 'procedures')['normalized_result']
    found = [x for x in found if x['value'] == name]
    if not found:
        raise SystemExit('REA did not find ' + name)
    addr = min(found, key=lambda h: int(h['address'], 16))['address']
    result = run('instructions', obj, addr)
    print(name, 'REA evidence:', result['evidence_id'])
    return result['normalized_result']['instructions']

def adjacent(ins, first, second):
    return any(first in ins[i].lower() and second in ins[i + 1].lower()
               for i in range(len(ins) - 1))

wake = procedure('ArmCpu::wakeRedistributor')
if not adjacent(wake, ': isb', 'cntvct_el0'):
    raise SystemExit('wakeRedistributor: missing ISB/CNTVCT read')
timer = procedure('ArmCpu::enableCPUTimeBase')
if not adjacent(timer, 'msr cntv_ctl_el0', ': isb'):
    raise SystemExit('enableCPUTimeBase: missing timer write/ISB sequence')
signal = procedure('ArmCpu::signalCPU')
if not any(': dsb ishst' in x.lower() for x in signal) or not any(': isb' in x.lower() for x in signal):
    raise SystemExit('signalCPU: missing GIC signal barriers')
print('PASS: GIC wake counter, timer enable and GIC signal barriers')
