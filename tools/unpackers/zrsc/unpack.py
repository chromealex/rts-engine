#!/usr/bin/env python3
"""Z (Bitmap Brothers) .RSC: u16 count, u32 offsets[count] (ascending), records up to the next
offset. Bytes between the table and the first record are kept as _prelude.bin. Records are
written as NNNN.bin (sprite decoding is an importer's job, not the unpacker's)."""
import os, struct, sys
sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry, log  # noqa: E402


def table(d):
    if len(d) < 6:
        return None
    n = struct.unpack_from('<H', d, 0)[0]
    if n == 0 or 2 + 4 * n > len(d):
        return None
    offs = list(struct.unpack_from('<%dI' % n, d, 2))
    if offs[0] < 2 + 4 * n or offs[-1] > len(d) or any(b < a for a, b in zip(offs, offs[1:])):
        return None
    return offs


def probe(path):
    with open(path, 'rb') as f:
        return table(f.read()) is not None


def entries(path):
    with open(path, 'rb') as f:
        d = f.read()
    offs = table(d)
    if offs is None:
        raise ValueError('not a Z .RSC file')
    start = 2 + 4 * len(offs)
    if offs[0] > start:
        pre = d[start:offs[0]]
        yield Entry('_prelude.bin', len(pre), lambda: pre)
    ends = offs[1:] + [len(d)]
    for i, (a, b) in enumerate(zip(offs, ends)):
        rec = d[a:b]
        yield Entry('%04d.bin' % i, len(rec), lambda rec=rec: rec)


if __name__ == '__main__':
    run('zrsc', '1.0.0', probe, entries)
