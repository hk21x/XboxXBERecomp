#!/usr/bin/env python3
"""Reads a guest memory dump that F3 writes with XBE_CAPTURE_DIR set (guest_mem_*.bin: "XBEMEM1\\0", then runs of
committed pages, each its guest address, its length and its bytes).
    scripts/guestmem.py dump.bin d ADDR [COUNT]    dwords from ADDR (hex)
    scripts/guestmem.py dump.bin x ADDR [LEN]      a hex dump
    scripts/guestmem.py dump.bin s ADDR            a C string
    scripts/guestmem.py dump.bin regions           the committed runs"""
import struct, sys


def load(path):
    data = open(path, 'rb').read()
    if data[:8] != b'XBEMEM1\0':
        sys.exit('not a guest memory dump')
    runs, at = [], 8
    while at + 8 <= len(data):
        va, n = struct.unpack_from('<II', data, at)
        runs.append((va, n, at + 8))
        at += 8 + n
    return data, runs


def read(data, runs, va, n):
    out = b''
    while n > 0:
        for base, size, at in runs:
            if base <= va < base + size:
                k = min(n, base + size - va)
                out += data[at + va - base:at + va - base + k]
                va += k
                n -= k
                break
        else:
            raise SystemExit(f'{va:08X} is not in the dump')
    return out


def main():
    data, runs = load(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'regions':
        for base, size, _ in runs:
            print(f'{base:08X}..{base + size:08X} {size >> 10} KB')
        return
    va = int(sys.argv[3], 16)
    if cmd == 'd':
        count = int(sys.argv[4]) if len(sys.argv) > 4 else 8
        for i in range(count):
            print(f'{va + i * 4:08X} {struct.unpack("<I", read(data, runs, va + i * 4, 4))[0]:08X}')
    elif cmd == 'x':
        n = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x40
        b = read(data, runs, va, n)
        for i in range(0, n, 16):
            print(f'{va + i:08X} {b[i:i + 16].hex(" ")}')
    elif cmd == 's':
        b = read(data, runs, va, 256)
        print(b.split(b'\0')[0].decode('latin-1'))


if __name__ == '__main__':
    main()
