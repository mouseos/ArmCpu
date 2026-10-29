#!/usr/bin/env python3
import importlib.util
import struct
from pathlib import Path

path = Path(__file__).resolve().parents[1] / 'tools/audit-pmu-macho.py'
spec = importlib.util.spec_from_file_location('audit_pmu', path)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

def image(words, prot=5):
    payload = b''.join(struct.pack('<I', w) for w in words)
    header = struct.pack('<IiiIIIII', 0xfeedfacf, 0x100000c, 0, 2, 1, 72, 0, 0)
    segment = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT_EXEC', 0x1000, len(payload), 104, len(payload), 7, prot, 0, 0)
    return header + segment + payload

# MRS PMCR; MSR MDCR_EL2; MRS CNTVCT (not PMU); MSR PMCCNTR.
fixture = image([0xd53b9c01, 0xd51c1121, 0xd53be040, 0xd51b9d09])
segments, hits = audit.scan(fixture)
assert len(segments) == 1 and [x['address'] for x in hits] == ['0x1000','0x1004','0x100c']
assert 'pmcr_el0' in hits[0]['instruction'] and 'mdcr_el2' in hits[1]['instruction'] and 'pmccntr_el0' in hits[2]['instruction']
assert audit.scan(image([0xd53b9c01], prot=1))[1] == []
for bad in (b'', fixture[:90], fixture[:103], fixture[:-1], bytes(32)):
    try:
        audit.scan(bad)
    except ValueError:
        pass
    else:
        raise AssertionError('malformed Mach-O accepted')
print('PASS: real PMU opcodes, non-PMU filtering, executable protection and malformed Mach-O refusal')
