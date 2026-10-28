#!/usr/bin/env python3
"""Westwood .PAK (Dune II, Lands of Lore, Legend of Kyrandia 1-2): a header of
(u32 LE offset, NUL-terminated 8.3 name) records, ended by a zero offset, by an offset equal
to the file size, or by reaching the first data offset. Each file runs to the next offset.
Files are stored uncompressed; their own formats (CPS, SHP, WSA, VOC, ...) are kept as is."""
import os
import struct
import sys

sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry  # noqa: E402

NAME_OK = set(b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-~!#$%&()@^{}')


def table(d):
    """[(name, offset, end)] or None if this isn't a Westwood PAK."""
    n, p, recs = len(d), 0, []
    first = None
    while True:
        if p + 4 > n:
            return None
        off = struct.unpack_from('<I', d, p)[0]
        if off == 0 or (off == n and recs and (p + 4 >= n or d[p + 4] == 0)):
            break  # terminator
        q = d.find(b'\0', p + 4, p + 4 + 64)
        if q < 0:
            return None
        name = d[p + 4:q]
        if not name or len(name) > 32 or any(c not in NAME_OK for c in name):
            return None
        if off > n or off < q + 1 or (recs and off < recs[-1][1]):
            return None
        recs.append((name.decode('ascii'), off))
        first = off if first is None else min(first, off)
        p = q + 1
        if p >= first:
            break  # header reached the data
    if not recs or first < p:
        return None
    ends = [o for _, o in recs[1:]] + [n]
    return [(nm, o, e) for (nm, o), e in zip(recs, ends)]


def probe(path):
    with open(path, 'rb') as f:
        head = f.read(4)
        if head[:4] in (b'PACK', b'PK\x03\x04'):
            return False  # Quake .pak, zip
        f.seek(0)
        return table(f.read()) is not None


def entries(path):
    with open(path, 'rb') as f:
        d = f.read()
    t = table(d)
    if t is None:
        raise ValueError('not a Westwood PAK')
    for name, a, b in t:
        blob = d[a:b]
        yield Entry(name, len(blob), lambda blob=blob: blob)


if __name__ == '__main__':
    run('wwpak', '1.0.0', probe, entries)
