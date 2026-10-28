#!/usr/bin/env python3
"""Self-test of the Westwood codecs on hand-made streams (no game data needed). Exit code 0 = ok."""
import os
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import westwood as ww  # noqa: E402


def check(cond, what):
    if not cond:
        print('FAIL:', what)
        sys.exit(1)


# Format80: literal "ABC", relative copy (3 from 3 back), fill 4 x 'Z', absolute copy 3 from 0, end.
f80 = bytes([0x83]) + b'ABC' + bytes([0x00, 0x03]) + bytes([0xFE]) + struct.pack('<HB', 4, ord('Z')) + bytes([0xC0]) + struct.pack('<H', 0) + bytes([0x80])
check(bytes(ww.format80(f80, 100)) == b'ABCABCZZZZABC', 'format80')
# Format40: xor 2 bytes, skip 1, xor-fill 2, end.
buf = bytearray(b'\x01\x01\x01\x01\x01\x01')
ww.format40(bytes([0x02, 0x03, 0x03, 0x81, 0x00, 0x02, 0xFF, 0x80, 0x00, 0x00]), buf)
check(bytes(buf) == b'\x02\x02\x01\xfe\xfe\x01', 'format40 %r' % bytes(buf))
# Format2 (SHP pixels): 2 colours, 3 transparent, 1 colour remapped.
check(bytes(ww.format2(bytes([5, 6, 0, 3, 1]), 6, 1, remap=bytes(range(16, 32)))) == bytes([21, 22, 0, 0, 0, 17]), 'format2')
# VOC -> PCM
voc = b'Creative Voice File\x1a' + struct.pack('<HHH', 26, 0x010A, 0x1129) + bytes([1, 6, 0, 0, 0xA6, 0]) + b'\x80\x81\x82\x83' + b'\0'
rate, ch, bits, pcm = ww.read_voc(voc)
check((rate, ch, bits, pcm) == (11111, 1, 8, b'\x80\x81\x82\x83'), 'voc %r' % ((rate, ch, bits, pcm),))
# Palette scaling: 6-bit 63 -> 255
check(ww.read_pal(bytes([63, 0, 32] * 256))[0] == (255, 0, 130), 'palette')
print('ok')
