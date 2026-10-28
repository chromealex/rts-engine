"""Images for the Unity effect importer, standard library only.

PNG read/write (8-bit, non-interlaced), TGA read (uncompressed / RLE, 8/16/24/32 bit), and conversion of
anything else (PSD, JPG, TIF, EXR, ...) through ImageMagick or ffmpeg when one is installed.
"""
import os
import shutil
import struct
import subprocess
import tempfile
import zlib


class Image:
    def __init__(self, w, h, rgba):
        self.w, self.h, self.rgba = w, h, bytearray(rgba)  # RGBA8, rows top to bottom

    def has_alpha(self):
        return any(self.rgba[i] != 255 for i in range(3, len(self.rgba), 4))


# ------------------------------------------------------------------------------------------ PNG

def _chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)


def write_png(path, img):
    stride = img.w * 4
    raw = b''.join(b'\0' + bytes(img.rgba[y * stride:(y + 1) * stride]) for y in range(img.h))
    data = (b'\x89PNG\r\n\x1a\n' + _chunk(b'IHDR', struct.pack('>IIBBBBB', img.w, img.h, 8, 6, 0, 0, 0)) +
            _chunk(b'IDAT', zlib.compress(raw, 9)) + _chunk(b'IEND', b''))
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def read_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte, trns = 8, b'', None, None
    w = h = depth = ctype = interlace = 0
    while pos < len(data):
        n, tag = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if tag == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif tag == b'PLTE':
            plte = body
        elif tag == b'tRNS':
            trns = body
        elif tag == b'IDAT':
            idat += body
        elif tag == b'IEND':
            break
    if interlace:
        raise ValueError('interlaced PNG')
    if depth not in (8, 16) and ctype != 3:
        raise ValueError('PNG bit depth %d' % depth)
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp_bits = chans * depth
    stride = (w * bpp_bits + 7) // 8
    bpp = max(1, bpp_bits // 8)
    raw = zlib.decompress(idat)
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif ft == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 255
        rows.append(line)
        prev = line
    out = bytearray(w * h * 4)
    for y, line in enumerate(rows):
        for x in range(w):
            o = (y * w + x) * 4
            if ctype == 3:
                bits = depth
                byte = line[(x * bits) // 8]
                idx = (byte >> (8 - bits - (x * bits) % 8)) & ((1 << bits) - 1)
                r, g, b_ = plte[idx * 3:idx * 3 + 3] if plte and idx * 3 + 2 < len(plte) else (0, 0, 0)
                a = trns[idx] if trns and idx < len(trns) else 255
                out[o:o + 4] = bytes((r, g, b_, a))
                continue
            step = depth // 8
            vals = [line[(x * chans + c) * step] for c in range(chans)]  # 16-bit: high byte
            if ctype == 0:
                out[o:o + 4] = bytes((vals[0], vals[0], vals[0], 255))
            elif ctype == 2:
                out[o:o + 4] = bytes((vals[0], vals[1], vals[2], 255))
            elif ctype == 4:
                out[o:o + 4] = bytes((vals[0], vals[0], vals[0], vals[1]))
            else:
                out[o:o + 4] = bytes(vals)
    return Image(w, h, out)


# ------------------------------------------------------------------------------------------ TGA

def read_tga(data):
    idlen, cmtype, itype = data[0], data[1], data[2]
    cmlen, cmdepth = struct.unpack('<H', data[5:7])[0], data[7]
    w, h, depth, desc = struct.unpack('<HHBB', data[12:18])
    pos = 18 + idlen
    cmap = None
    if cmtype == 1:
        cbytes = (cmdepth + 7) // 8
        cmap = data[pos:pos + cmlen * cbytes]
        pos += cmlen * cbytes
    if itype not in (1, 2, 3, 9, 10, 11):
        raise ValueError('TGA image type %d' % itype)
    bpp = (depth + 7) // 8
    n = w * h
    pixels = bytearray()
    if itype in (9, 10, 11):
        while len(pixels) < n * bpp:
            hdr = data[pos]
            pos += 1
            count = (hdr & 0x7F) + 1
            if hdr & 0x80:
                pixels += data[pos:pos + bpp] * count
                pos += bpp
            else:
                pixels += data[pos:pos + bpp * count]
                pos += bpp * count
    else:
        pixels = bytearray(data[pos:pos + n * bpp])

    def px(i):
        v = pixels[i * bpp:(i + 1) * bpp]
        if itype in (1, 9) and cmap is not None:
            cb = (cmdepth + 7) // 8
            idx = v[0]
            v = cmap[idx * cb:(idx + 1) * cb]
            return _tga_color(v, cmdepth)
        if itype in (3, 11):
            g = v[0]
            return (g, g, g, v[1] if bpp > 1 else 255)
        return _tga_color(v, depth)

    out = bytearray(n * 4)
    top = bool(desc & 0x20)
    right = bool(desc & 0x10)
    for y in range(h):
        sy = y if top else h - 1 - y
        for x in range(w):
            sx = w - 1 - x if right else x
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(px(sy * w + sx))
    return Image(w, h, out)


def _tga_color(v, depth):
    if depth == 32:
        return (v[2], v[1], v[0], v[3])
    if depth == 24:
        return (v[2], v[1], v[0], 255)
    if depth in (15, 16):
        p = v[0] | (v[1] << 8)
        r, g, b = (p >> 10) & 31, (p >> 5) & 31, p & 31
        return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255 if depth == 15 or p & 0x8000 else 0)
    return (v[0], v[0], v[0], 255)


def write_tga(path, img, rle=False):
    """Test helper: 32-bit TGA (top-left origin)."""
    out = bytearray(struct.pack('<BBBHHBHHHHBB', 0, 0, 10 if rle else 2, 0, 0, 0, 0, 0, img.w, img.h, 32, 0x28))
    bgra = bytearray()
    for i in range(0, len(img.rgba), 4):
        r, g, b, a = img.rgba[i:i + 4]
        bgra += bytes((b, g, r, a))
    if not rle:
        out += bgra
    else:
        i, n = 0, img.w * img.h
        while i < n:
            run = 1
            while i + run < n and run < 128 and bgra[(i + run) * 4:(i + run + 1) * 4] == bgra[i * 4:(i + 1) * 4]:
                run += 1
            if run > 1:
                out += bytes((0x80 | (run - 1),)) + bgra[i * 4:(i + 1) * 4]
            else:
                out += bytes((0,)) + bgra[i * 4:(i + 1) * 4]
            i += run
    with open(path, 'wb') as f:
        f.write(out)


# ------------------------------------------------------------------------------------------ others

def external_tool():
    for t in ('magick', 'convert', 'ffmpeg'):
        p = shutil.which(t)
        if p:
            return t, p
    return None, None


def convert_external(src, dst_png):
    """PSD/JPG/TIF/EXR/... -> PNG with ImageMagick or ffmpeg. Returns an error string or None."""
    name, exe = external_tool()
    if not exe:
        return 'no image converter found (install ImageMagick or ffmpeg)'
    src_arg = src + '[0]' if name in ('magick', 'convert') and src.lower().endswith(('.psd', '.psb', '.tif', '.tiff')) else src
    if name == 'ffmpeg':
        cmd = [exe, '-v', 'error', '-y', '-i', src, '-frames:v', '1', '-pix_fmt', 'rgba', dst_png]
    elif name == 'magick':
        cmd = [exe, src_arg, '-background', 'none', 'PNG32:' + dst_png]
    else:
        cmd = [exe, src_arg, '-background', 'none', 'PNG32:' + dst_png]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as e:
        return str(e)
    if r.returncode != 0 or not os.path.exists(dst_png):
        return (r.stderr.decode('utf-8', 'replace').strip() or 'conversion failed')[:300]
    return None


def load_any(path):
    """-> (Image or None, note or None). Notes say how the file was decoded or why it could not be."""
    ext = os.path.splitext(path)[1].lower()
    with open(path, 'rb') as f:
        data = f.read()
    if ext == '.png':
        try:
            return read_png(data), None
        except Exception as e:  # odd PNGs (16-bit gray + alpha, interlaced): let an external tool try
            err = str(e)
    elif ext == '.tga':
        try:
            return read_tga(data), None
        except Exception as e:
            err = str(e)
    else:
        err = None
    with tempfile.TemporaryDirectory() as tmp:
        dst = os.path.join(tmp, 'out.png')
        e2 = convert_external(path, dst)
        if e2:
            return None, (err + '; ' if err else '') + e2
        with open(dst, 'rb') as f:
            try:
                return read_png(f.read()), 'converted with ' + external_tool()[0]
            except Exception as e3:
                return None, 'converted file unreadable: %s' % e3


def alpha_from_gray(img):
    for i in range(0, len(img.rgba), 4):
        r, g, b = img.rgba[i:i + 3]
        img.rgba[i + 3] = (r * 77 + g * 151 + b * 28) >> 8
    return img


def crop(img, x, y, w, h):
    """Sub-image (x, y from the top left), clamped to the image."""
    x, y = max(0, min(img.w - 1, x)), max(0, min(img.h - 1, y))
    w, h = max(1, min(img.w - x, w)), max(1, min(img.h - y, h))
    rows = [bytes(img.rgba[((y + r) * img.w + x) * 4:((y + r) * img.w + x + w) * 4]) for r in range(h)]
    return Image(w, h, b''.join(rows))
