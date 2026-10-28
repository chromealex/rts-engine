#!/usr/bin/env python3
"""Z (Bitmap Brothers) .PAC: 4-byte header (zero) + LZSS stream, one file per .PAC.
Flag byte MSB-first, 1 = literal; reference = 2 bytes b0 b1: length (b0 & 15) + 2,
ring position (b1 << 4 | b0 >> 4) in a 4 KB zero-filled ring written from position 1.
Verified: DMAIN.PAC unpacks to a stream starting with the demo's PHRASES.BIN byte-for-byte.
Output is named after the archive, with an extension guessed from content (.lbm for IFF)."""
import os, struct, sys
sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry, log  # noqa: E402


def unpac(d):
    out, ring, r, p, n = bytearray(), bytearray(4096), 1, 4, len(d)
    while p < n:
        f = d[p]
        p += 1
        for _ in range(8):
            if p >= n:
                break
            if f & 0x80:
                c = d[p]
                p += 1
                out.append(c)
                ring[r] = c
                r = (r + 1) & 0xFFF
            else:
                if p + 1 >= n:
                    p = n
                    break
                b0, b1 = d[p], d[p + 1]
                p += 2
                pos = (b1 << 4) | (b0 >> 4)
                for k in range((b0 & 15) + 2):
                    c = ring[(pos + k) & 0xFFF]
                    out.append(c)
                    ring[r] = c
                    r = (r + 1) & 0xFFF
            f = (f << 1) & 0xFF
    if out[:4] == b'FORM' and len(out) >= 8:  # the stream pads past the IFF end
        size = 8 + struct.unpack('>I', bytes(out[4:8]))[0]
        if size <= len(out):
            del out[size + (size & 1):]
    return bytes(out)


def probe(path):
    with open(path, 'rb') as f:
        head = f.read(64)
    return len(head) > 8 and head[:4] == b'\0\0\0\0'


def entries(path):
    with open(path, 'rb') as f:
        data = f.read()
    if not probe(path):
        raise ValueError('not a Z .PAC file')
    out = unpac(data)
    base = os.path.splitext(os.path.basename(path))[0]
    ext = '.lbm' if out[:4] == b'FORM' else '.bin'
    yield Entry(base + ext, len(out), lambda: out)


if __name__ == '__main__':
    run('zpac', '1.0.0', probe, entries)
