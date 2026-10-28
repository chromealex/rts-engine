#!/usr/bin/env python3
"""Blizzard Warcraft I/II archives: u32 magic (0x18 WC1, 0x19 WC2), u16 count, u16 archive id,
u32 offsets[count]; each entry: u32 (size:24 | flags:8), flags 0x20 = LZ (4 KB ring, 12-bit
position, 4-bit length + 3, LSB-first flag bits), 0x00 = stored. Offsets closer than 4 bytes to
the next one are placeholders. Entries are named NNNN.<ext> by content (wav, xmi, pud, ilbm)."""
import os, struct, sys
sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry, log  # noqa: E402

MAGICS = (0x18, 0x19)


def table(data):
    if len(data) < 8:
        return None
    magic, count, _aid = struct.unpack_from('<IHH', data, 0)
    if magic not in MAGICS or count == 0 or 8 + 4 * count > len(data):
        return None
    offs = list(struct.unpack_from('<%dI' % count, data, 8))
    if any(o > len(data) for o in offs if o != 0xFFFFFFFF):
        return None
    return offs


def decode(data, off):
    hdr = struct.unpack_from('<I', data, off)[0]
    size, flags = hdr & 0xFFFFFF, hdr >> 24
    p = off + 4
    if flags == 0x00:
        return data[p:p + size]
    if flags != 0x20:
        raise ValueError('unknown entry flags 0x%02x at 0x%x' % (flags, off))
    out, ring, ri, n = bytearray(), bytearray(4096), 0, len(data)
    while len(out) < size and p < n:
        bits = data[p]
        p += 1
        for _ in range(8):
            if bits & 1:
                c = data[p]
                p += 1
                out.append(c)
                ring[ri & 0xFFF] = c
                ri += 1
            else:
                w = data[p] | data[p + 1] << 8
                p += 2
                length, pos = (w >> 12) + 3, w & 0xFFF
                while length and len(out) < size:
                    c = ring[pos & 0xFFF]
                    pos += 1
                    out.append(c)
                    ring[ri & 0xFFF] = c
                    ri += 1
                    length -= 1
            if len(out) >= size:
                break
            bits >>= 1
    if len(out) != size:
        raise ValueError('truncated entry at 0x%x' % off)
    return bytes(out)


def ext(b):
    if b[:4] == b'RIFF':
        return '.wav'
    if b[:4] == b'FORM':
        return '.xmi' if b[8:12] in (b'XDIR', b'XMID') else '.iff'
    if b[:4] == b'TYPE':
        return '.pud'
    return '.bin'


def probe(path):
    with open(path, 'rb') as f:
        return table(f.read()) is not None


def entries(path):
    with open(path, 'rb') as f:
        data = f.read()
    offs = table(data)
    if offs is None:
        raise ValueError('not a Warcraft archive')
    ends = sorted(set(o for o in offs if o != 0xFFFFFFFF)) + [len(data)]
    for i, o in enumerate(offs):
        if o in (0, 0xFFFFFFFF):
            continue
        nxt = next(e for e in ends if e > o) if o < len(data) else o
        if nxt - o < 4:
            continue  # placeholder entry
        try:
            b = decode(data, o)
        except (ValueError, IndexError) as ex:
            log('entry %d: %s' % (i, ex))
            continue
        yield Entry('%04d%s' % (i, ext(b)), len(b), lambda b=b: b)


if __name__ == '__main__':
    run('war', '1.0.0', probe, entries)
