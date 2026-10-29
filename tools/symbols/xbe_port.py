#!/usr/bin/env python3
# Maps addresses from one release of a game's xbe to another build of the same game (another region's compile):
# code by its instructions, numbers that point into the image masked, data by the operands of code that matched.
#   python3 sdk/tools/symbols/xbe_port.py from.xbe to.xbe 0x7C960 0x3722B0 ...      one line per address
#   python3 sdk/tools/symbols/xbe_port.py from.xbe to.xbe --file addresses.txt
#   python3 sdk/tools/symbols/xbe_port.py from.xbe to.xbe --toml from.toml to.toml   a recompiler config's
#     [data] / [symbols] / [functions] values ported, other lines kept; a value it cannot place is kept and marked
# Each line: from, to (or ?), how (code: the instructions matched before / after; data: votes), and the from
# address's section. Needs objdump. Only a unique match is given; an address the tool cannot place shows ?.
import bisect
import re
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

WINDOW = 12  # instructions a code address is matched by, starting at it


def sections(path):
    d = Path(path).read_bytes()
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]
    base = u32(0x104)
    n, sh = u32(0x11C), u32(0x120) - base
    out = []
    for i in range(n):
        flags, va, vsize, raw, rsize, name = struct.unpack_from('<6I', d, sh + i * 56)
        nm = d[name - base:name - base + 16].split(b'\0')[0].decode()
        data = d[raw:raw + rsize] + bytes(max(0, vsize - rsize))
        # code: executable sections but the data ones (an xbe marks .rdata and .data executable too)
        data_names = ('.rdata', '.data', '.XTLID', 'DOLBY', '$$XTIMAGE', 'XON_RD', '.idata')
        out.append((nm, va, data, bool(flags & 4) and not nm.startswith(data_names)))
    return base, out


NUM = re.compile(r'0x[0-9a-f]+')


class Image:
    def __init__(self, path):
        self.base, self.secs = sections(path)
        self.lo = min(va for _, va, _, _ in self.secs)
        self.hi = max(va + len(d) for _, va, d, _ in self.secs)
        self.insns = []  # (va, size, text, norm)
        for nm, va, data, code in self.secs:
            if code:
                self._disasm(va, data)
        self.insns.sort()
        self.vas = [i[0] for i in self.insns]

    def in_image(self, v):
        return self.lo <= v < self.hi

    def section(self, v):
        for nm, va, d, _ in self.secs:
            if va <= v < va + len(d):
                return nm
        return '?'

    def _disasm(self, va, data):
        with tempfile.NamedTemporaryFile(suffix='.bin') as f:
            f.write(data)
            f.flush()
            out = subprocess.run(['objdump', '-D', '-b', 'binary', '-m', 'i386', '-M', 'intel', '--adjust-vma',
                                  hex(va), f.name], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$', line)
            if not m:
                continue
            a = int(m.group(1), 16)
            size = len(m.group(2).split())
            text = m.group(3).split('#')[0].strip()
            self.insns.append((a, size, text, self.norm(text)))

    def norm(self, text):
        mnem = text.split(' ')[0]
        if mnem in ('call', 'jmp') or mnem.startswith('j') or mnem.startswith('loop'):
            # a branch: its kind, not where
            return mnem + (' [ind]' if '[' in text or 'DWORD' in text else ' REL') if NUM.search(text) else text
        return NUM.sub(lambda m: 'A' if self.in_image(int(m.group(0), 16)) else m.group(0), text)

    def index_of(self, va):
        i = bisect.bisect_left(self.vas, va)
        return i if i < len(self.vas) and self.vas[i] == va else None


def window(img, i, n=WINDOW):
    return tuple(x[3] for x in img.insns[i:i + n])


def port_code(src, dst, dst_index, va):
    i = src.index_of(va)
    if i is None:
        return None, 'not an instruction start in the from xbe'
    key = window(src, i)
    cands = dst_index.get(key, [])
    if len(cands) == 1:
        return dst.insns[cands[0]][0], f'{WINDOW} instructions'
    # grow the window forward until one candidate stays, and also try the instructions before it
    for n in (24, 48, 96):
        k = window(src, i, n)
        left = [c for c in cands if window(dst, c, n) == k]
        if len(left) == 1:
            return dst.insns[left[0]][0], f'{n} instructions'
        if not left:
            break
        cands = left
    if len(cands) > 1:
        # before it as well
        for back in (8, 24):
            if i < back:
                break
            kb = window(src, i - back, back)
            left = [c for c in cands if c >= back and window(dst, c - back, back) == kb]
            if len(left) == 1:
                return dst.insns[left[0]][0], f'{WINDOW} after, {back} before'
        return None, f'{len(cands)} places match'
    return None, 'no match'


def operands(text):
    return [int(x, 16) for x in NUM.findall(text)]


def port_data(src, dst, dst_index, va, limit=40):
    # every instruction of the from xbe naming va (as a number in its text): its code ported, the number at the same
    # place in the to xbe's instruction voted for, offsets inside a structure kept (va - k in the operand)
    votes = defaultdict(int)
    seen = 0
    for i, (a, size, text, norm) in enumerate(src.insns):
        nums = operands(text)
        hits = [x for x in nums if x == va or (0 <= va - x < 0x100 and src.in_image(x))]
        if not hits or 'A' not in norm:
            continue
        # the instruction's function context: match a window starting a few instructions before it
        start = max(0, i - 6)
        key = window(src, start)
        cands = dst_index.get(key, [])
        if len(cands) != 1:
            continue
        j = cands[0] + (i - start)
        if j >= len(dst.insns) or dst.insns[j][3] != norm:
            continue
        dnums = operands(dst.insns[j][2])
        if len(dnums) != len(nums):
            continue
        for x, y in zip(nums, dnums):
            if x in hits:
                votes[y + (va - x)] += 1
        seen += 1
        if seen >= limit:
            break
    if not votes:
        return None, 'no instruction that names it matched'
    best = sorted(votes.items(), key=lambda kv: -kv[1])
    if len(best) > 1 and best[1][1] * 2 >= best[0][1]:
        return None, 'votes split ' + ', '.join(f'{hex(k)}:{v}' for k, v in best[:3])
    return best[0][0], f'{best[0][1]} of {sum(votes.values())} votes'


def main():
    args = sys.argv[1:]
    if len(args) < 3:
        sys.exit(__doc__ if __doc__ else 'usage: xbe_port.py from.xbe to.xbe addr... | --file list')
    src, dst = Image(args[0]), Image(args[1])
    toml_in = toml_out = None
    addrs = []
    if args[2] == '--toml':
        toml_in, toml_out = args[3], args[4]
    elif args[2] == '--file':
        addrs = [int(t, 16) for t in re.findall(r'0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{6,8}\b', Path(args[3]).read_text())]
    else:
        addrs = [int(a, 16) for a in args[2:]]
    if toml_out:
        pass
    dst_index = defaultdict(list)
    for i in range(len(dst.insns) - WINDOW):
        dst_index[window(dst, i)].append(i)

    def port(va):
        if src.index_of(va) is not None:
            return port_code(src, dst, dst_index, va)
        return port_data(src, dst, dst_index, va)

    if toml_out:
        lines, section, missed = [], '', 0
        for line in Path(toml_in).read_text().splitlines():
            m = re.match(r'^(\s*\[+[^\]]+\]+)', line)
            if m:
                section = m.group(1).strip('[] ')
            v = re.match(r'^(\s*[A-Za-z_][\w]*\s*=\s*)(0x[0-9A-Fa-f]+)(.*)$', line)
            if v and section in ('data', 'symbols', 'functions'):
                to, how = port(int(v.group(2), 16))
                if to is None:
                    missed += 1
                    line = f'{line}  # NOT PORTED: {how}'
                else:
                    width = len(v.group(2)) - 2
                    line = f'{v.group(1)}{to:#0{width + 2}X}'.replace('0X', '0x') + v.group(3)
            lines.append(line)
        Path(toml_out).write_text('\n'.join(lines) + '\n')
        print(f'{toml_out}: {missed} values not ported')
        return
    for va in addrs:
        sec = src.section(va)
        if src.index_of(va) is not None:
            to, how = port_code(src, dst, dst_index, va)
        else:
            to, how = port_data(src, dst, dst_index, va)
        print(f'{va:#010x} {(f"{to:#010x}" if to is not None else "?"):>10}  {sec:7} {how}')


main()
