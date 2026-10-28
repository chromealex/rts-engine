"""Minimal PNG writer/reader (standard library only): 8-bit RGBA and grayscale."""
import os
import struct
import zlib


def _chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, width, height, pixels, channels=4):
    """pixels: bytes of width*height*channels (RGBA or gray). Deterministic output (no timestamps)."""
    color = {1: 0, 3: 2, 4: 6}[channels]
    stride = width * channels
    raw = b''.join(b'\0' + bytes(pixels[y * stride:(y + 1) * stride]) for y in range(height))
    data = (b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, color, 0, 0, 0)) +
            _chunk(b'IDAT', zlib.compress(raw, 9)) + _chunk(b'IEND', b''))
    if os.path.dirname(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    return len(data)
