#!/usr/bin/env python3
"""Inventory architectural PMU system-register instructions in executable Mach-O segments.

Requires Python capstone. Static hits include unreachable instructions; an empty
result cannot rule out runtime-generated code, aliases or higher-EL agents.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

REGS = {
    'pmcr_el0', 'pmccntr_el0', 'pmccfiltr_el0', 'pmcntenset_el0',
    'pmcntenclr_el0', 'pmintenset_el1', 'pmintenclr_el1', 'pmovsclr_el0',
    'pmovsset_el0', 'pmuserenr_el0', 'pmselr_el0', 'pmxevtyper_el0',
    'pmxevcntr_el0', 'mdcr_el2', 'mdcr_el3',
}

def executable_segments(data):
    if len(data) < 32:
        raise ValueError('short Mach-O header')
    magic, cpu, _, _, ncmds, cmdbytes, _, _ = struct.unpack_from('<IiiIIIII', data)
    if magic != 0xfeedfacf or cpu != 0x100000c:
        raise ValueError('expected little-endian arm64 Mach-O')
    end = 32 + cmdbytes
    if end > len(data):
        raise ValueError('load commands exceed file')
    offset = 32
    for _ in range(ncmds):
        if offset + 8 > end:
            raise ValueError('truncated load command')
        cmd, size = struct.unpack_from('<II', data, offset)
        if size < 8 or offset + size > end:
            raise ValueError('invalid load command size')
        if cmd == 0x19:
            if size < 72:
                raise ValueError('short segment')
            _, _, rawname, addr, _, fileoff, filesize, _, prot, nsects, _ = struct.unpack_from('<II16sQQQQiiII', data, offset)
            if 72 + nsects * 80 > size or fileoff + filesize > len(data):
                raise ValueError('segment outside file')
            if prot & 4 and filesize:
                if fileoff % 4 or filesize % 4:
                    raise ValueError('unaligned executable segment')
                yield rawname.split(b'\0')[0].decode('ascii'), addr, fileoff, filesize
        offset += size
    if offset != end:
        raise ValueError('load command count mismatch')

def scan(data):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    hits = []
    segments = list(executable_segments(data))
    for name, addr, off, size in segments:
        for index, (word,) in enumerate(struct.iter_unpack('<I', memoryview(data)[off:off+size])):
            # MRS and MSR (register): discard the Rt and system-register fields.
            if word & 0xffe00000 not in (0xd5200000, 0xd5000000):
                continue
            ins = next(md.disasm(struct.pack('<I', word), addr + 4 * index), None)
            if ins and ins.mnemonic in ('mrs', 'msr'):
                operands = [x.strip() for x in ins.op_str.lower().split(',')]
                reg = operands[1] if ins.mnemonic == 'mrs' and len(operands) == 2 else operands[0]
                if reg in REGS or reg.startswith(('pmevcntr', 'pmevtyper')):
                    hits.append({'segment': name, 'address': hex(ins.address), 'instruction': f'{ins.mnemonic} {ins.op_str}', 'word': hex(word)})
    return segments, hits

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('macho', type=Path)
    args = p.parse_args()
    data = args.macho.read_bytes()
    segments, hits = scan(data)
    print(json.dumps({'sha256': hashlib.sha256(data).hexdigest(), 'segments': [dict(name=n, address=hex(a), file_offset=hex(o), size=s) for n,a,o,s in segments], 'hits': hits, 'limitations': 'Static executable-segment inventory; neither ownership nor upper-EL safety is established.'}, indent=2))

if __name__ == '__main__':
    main()
