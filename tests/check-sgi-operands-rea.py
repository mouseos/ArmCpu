#!/usr/bin/env python3
"""Compile the actual SGI inline asm and verify its operand flow with REA."""
import json
import re
import subprocess
import tempfile
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'ArmCpu.cpp').read_text()
lines = [line.strip() for line in source.splitlines()
         if '__asm__ volatile' in line and 'icc_sgi1r_el1' in line and '(tb)' in line]
if len(lines) != 1:
    raise SystemExit('expected one SGI/timestamp assembly statement')
# Use sgi after the asm to force the compiler to preserve the input separately
# from the output, making an operand reversal observable in the instructions.
with tempfile.TemporaryDirectory(prefix='armcpu-sgi-rea-') as tmp:
    cpp = Path(tmp) / 'probe.cpp'
    obj = Path(tmp) / 'probe.o'
    cpp.write_text('typedef unsigned long long uint64_t;\n'
                   'extern "C" uint64_t sgi_operand_test(uint64_t sgi) { uint64_t tb;\n'
                   + lines[0] + '\nreturn tb + sgi; }\n')
    subprocess.run(['clang++', '-target', 'arm64-apple-ios17.0', '-O2', '-c', str(cpp), '-o', str(obj)], check=True)
    data = json.loads(subprocess.check_output(['rea', 'instructions', str(obj), '_sgi_operand_test',
                                                '--provider', 'ghidra', '--format', 'json'], text=True))
    ins = data['normalized_result']['instructions']
    writes = [re.search(r'msr\s+(?:icc_sgi1r_el1|sreg\(0x3, 0x0, c0xc, c0xb, 0x5\)),\s*(x\d+)', x, re.I) for x in ins]
    reads = [re.search(r'mrs\s+(x\d+),\s*cntvct_el0', x, re.I) for x in ins]
    writes = [x for x in writes if x]; reads = [x for x in reads if x]
    if len(writes) != 1 or len(reads) != 1 or writes[0].group(1) != 'x0' or reads[0].group(1) == 'x0':
        raise SystemExit('SGI input/timestamp output register flow is incorrect: ' + repr(ins))
    print('PASS: SGI input x0 is written and the timestamp is read into a distinct output register')
    print('REA evidence:', data['evidence_id'])
