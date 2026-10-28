"""Westwood Studios data formats (Dune II era), standard library only.

Codecs: Format80 (LCW), Format40 (XOR delta), Format2 (zero-run RLE used inside SHP frames).
Files: PAL, CPS, SHP (Dune II v1.00 and v1.07 offset tables, remap tables), WSA, ICN (IFF ICON:
SINF/SSET/RPAL/RTBL) with ICON.MAP, VOC (Creative Voice -> PCM), string tables (.ENG/.FRE/.GER).
Written from the public format descriptions; every decoder is checked against real game files.
"""
import struct


class FormatError(ValueError):
    pass


# ------------------------------------------------------------------ codecs

def format80(src, size, start=0):
    """LCW decompression into `size` bytes. Absolute back-references into the output buffer."""
    dst = bytearray()
    i, n = start, len(src)
    while i < n and len(dst) < size:
        c = src[i]
        i += 1
        if c == 0x80:
            break
        if not c & 0x80:                         # 0cccpppp p: copy cnt+3 from dst[-rel]
            cnt = ((c & 0x70) >> 4) + 3
            rel = ((c & 0x0F) << 8) | src[i]
            i += 1
            if rel == 0 or rel > len(dst):
                raise FormatError('format80: bad relative offset')
            for _ in range(cnt):
                dst.append(dst[-rel])
        elif not c & 0x40:                       # 10cccccc: literal run
            cnt = c & 0x3F
            dst += src[i:i + cnt]
            i += cnt
        elif c == 0xFE:                          # fill
            cnt, v = struct.unpack_from('<HB', src, i)
            i += 3
            dst += bytes([v]) * cnt
        else:                                    # 11cccccc pppp / 0xFF cccc pppp: absolute copy
            if c == 0xFF:
                cnt, pos = struct.unpack_from('<HH', src, i)
                i += 4
            else:
                cnt = (c & 0x3F) + 3
                pos = struct.unpack_from('<H', src, i)[0]
                i += 2
            for k in range(cnt):
                if pos + k >= len(dst):
                    raise FormatError('format80: copy beyond output')
                dst.append(dst[pos + k])
    if len(dst) > size:
        del dst[size:]
    return dst


def format40(src, dst, start=0):
    """XOR delta applied in place to `dst` (bytearray)."""
    i, p, n = start, 0, len(src)
    while i < n:
        c = src[i]
        i += 1
        if c == 0:                               # 00 cnt val: xor fill
            cnt, v = src[i], src[i + 1]
            i += 2
            for k in range(cnt):
                dst[p + k] ^= v
            p += cnt
        elif not c & 0x80:                       # 0ccccccc: xor next c bytes
            for k in range(c):
                dst[p + k] ^= src[i + k]
            i += c
            p += c
        elif c != 0x80:                          # 1ccccccc: skip
            p += c & 0x7F
        else:                                    # 80 wwww: long command
            w = struct.unpack_from('<H', src, i)[0]
            i += 2
            if w == 0:
                break
            if not w & 0x8000:                   # skip
                p += w
            elif not w & 0x4000:                 # xor next (w & 3FFF) bytes
                cnt = w & 0x3FFF
                for k in range(cnt):
                    dst[p + k] ^= src[i + k]
                i += cnt
                p += cnt
            else:                                # xor fill
                cnt = w & 0x3FFF
                v = src[i]
                i += 1
                for k in range(cnt):
                    dst[p + k] ^= v
                p += cnt
    return dst


def format2(src, width, height, remap=None):
    """Dune II SHP pixel stream: 0 followed by a count = transparent run, other bytes = colour."""
    out = bytearray(width * height)
    i, p, n, total = 0, 0, len(src), width * height
    while i < n and p < total:
        c = src[i]
        i += 1
        if c == 0:
            p += src[i] if i < n else 0
            i += 1
        else:
            out[p] = remap[c] if remap is not None and c < len(remap) else c
            p += 1
    return out


# ------------------------------------------------------------------ palette

def read_pal(data):
    """6-bit VGA palette (768 bytes) -> list of 256 (r, g, b) 8-bit tuples."""
    if len(data) < 768:
        raise FormatError('palette: %d bytes, need 768' % len(data))
    def c(v):
        v &= 0x3F
        return (v << 2) | (v >> 4)
    return [(c(data[3 * k]), c(data[3 * k + 1]), c(data[3 * k + 2])) for k in range(256)]


def to_rgba(indices, pal, transparent=0):
    out = bytearray(len(indices) * 4)
    for k, v in enumerate(indices):
        if v == transparent:
            continue
        r, g, b = pal[v]
        out[4 * k:4 * k + 4] = bytes((r, g, b, 255))
    return out


# ------------------------------------------------------------------ CPS (320x200 pictures)

def read_cps(data):
    """-> (pixels[64000], palette or None)."""
    if len(data) < 10:
        raise FormatError('cps: too small')
    _size, comp, usize, palsize = struct.unpack_from('<HHIH', data, 0)
    pal = read_pal(data[10:10 + palsize]) if palsize >= 768 else None
    body = 10 + palsize
    if comp == 4:
        px = format80(data, usize, body)
    elif comp == 0:
        px = bytearray(data[body:body + usize])
    else:
        raise FormatError('cps: compression %d not supported' % comp)
    if len(px) < usize:
        raise FormatError('cps: truncated image')
    return px, pal


# ------------------------------------------------------------------ SHP (sprites)

def shp_offsets(data):
    n = struct.unpack_from('<H', data, 0)[0]
    if n == 0:
        return []
    # v1.07 uses u32 offsets relative to byte 2; v1.00 uses u16 absolute-ish offsets. The high
    # word of the first u32 offset is always 0 in v1.07 files.
    if len(data) >= 6 + 4 * n and struct.unpack_from('<H', data, 4)[0] == 0:
        offs = struct.unpack_from('<%dI' % (n + 1), data, 2)
        return [(o + 2, n) for o in offs[:n]]
    offs = struct.unpack_from('<%dH' % (n + 1), data, 2)
    return [(o, n) for o in offs[:n]]


def read_shp(data):
    """-> list of frames: dict(width, height, pixels (palette indices, 0 = transparent), flags)."""
    frames = []
    for off, _n in shp_offsets(data):
        if off + 10 > len(data):
            raise FormatError('shp: frame offset out of range')
        flags, slices, width, height, fsize, zsize = struct.unpack_from('<HBHBHH', data, off)
        p = off + 10
        remap = None
        if flags & 1:
            rlen = 16
            if flags & 4:
                rlen = data[p]
                p += 1
            remap = data[p:p + rlen]
            p += rlen
        body = data[p:off + fsize]
        if not flags & 2:
            body = format80(body, zsize)
        px = format2(body, width, height, remap)
        frames.append({'width': width, 'height': height, 'pixels': px, 'flags': flags})
    return frames


# ------------------------------------------------------------------ WSA (animations)

def read_wsa(data, previous=None):
    """-> (width, height, frames[list of pixel buffers], loops). Dune II v1.00/v1.07 header.
    A first-frame offset of 0 means the animation continues `previous` (or starts blank)."""
    frames_n, width, height, _delta = struct.unpack_from('<HHHH', data, 0)
    # v1.07 has an extra u16 flags field; detect by where a sane offset table starts.
    for hdr in (10, 8):
        offs = list(struct.unpack_from('<%dI' % (frames_n + 2), data, hdr))
        table_end = hdr + 4 * (frames_n + 2)
        first = next((o for o in offs if o), None)
        if first is not None and first >= table_end and all(o <= len(data) for o in offs):
            break
    else:
        raise FormatError('wsa: bad offset table')
    has_pal = hdr == 10 and struct.unpack_from('<H', data, 8)[0] & 1
    shift = 768 if has_pal and offs[0] and offs[0] >= table_end + 768 else 0
    buf = bytearray(previous) if previous is not None else bytearray(width * height)
    out = []
    for k in range(frames_n):
        o, nxt = offs[k], offs[k + 1]
        if o:
            delta = format80(data, 0x10000, o + shift if shift and k == 0 else o)
            format40(delta, buf)
        out.append(bytes(buf))
    loops = bool(offs[frames_n + 1])
    return width, height, out, loops


# ------------------------------------------------------------------ ICN tiles + ICON.MAP

def _iff_chunks(data, start, end):
    p, out = start, {}
    while p + 8 <= end:
        tag, size = data[p:p + 4], struct.unpack_from('>I', data, p + 4)[0]
        out[tag] = data[p + 8:p + 8 + size]
        p += 8 + size + (size & 1)
    return out


def read_icn(data):
    """ICON.ICN -> (tile_w, tile_h, tiles[list of 4-bit index buffers mapped to palette indices])."""
    if data[:4] != b'FORM' or data[8:12] != b'ICON':
        raise FormatError('icn: not an IFF ICON file')
    ch = _iff_chunks(data, 12, 8 + struct.unpack_from('>I', data, 4)[0])
    sinf, sset, rpal, rtbl = ch.get(b'SINF'), ch.get(b'SSET'), ch.get(b'RPAL'), ch.get(b'RTBL')
    if not (sinf and sset and rpal and rtbl):
        raise FormatError('icn: missing SINF/SSET/RPAL/RTBL')
    tw, th, shift, bits = sinf[0], sinf[1], sinf[2], sinf[3]
    tw, th = tw << shift, th << shift
    # SSET: 8-byte header (u16 compression?, u16 ?, u32 size), then Format80 or raw 4bpp data.
    comp = struct.unpack_from('<H', sset, 0)[0]
    usize = struct.unpack_from('<I', sset, 4)[0] if len(sset) >= 8 else 0
    raw = format80(sset, usize, 8) if comp == 1 and usize else bytearray(sset[8:])
    per = tw * th * bits // 8
    count = len(raw) // per
    tiles = []
    for t in range(count):
        chunk = raw[t * per:(t + 1) * per]
        idx = rtbl[t] if t < len(rtbl) else 0
        pal16 = rpal[idx * 16:idx * 16 + 16]
        px = bytearray(tw * th)
        for k, b in enumerate(chunk):
            px[2 * k] = pal16[b >> 4]
            px[2 * k + 1] = pal16[b & 15]
        tiles.append(px)
    return tw, th, tiles


def read_icon_map(data):
    """ICON.MAP -> list of groups, each a list of tile indices (u16 offsets in words)."""
    words = struct.unpack_from('<%dH' % (len(data) // 2), data, 0)
    starts = []
    for w in words:
        if w == 0 or (starts and w <= starts[-1]) or w > len(words):
            break
        starts.append(w)
        if len(starts) >= words[0]:
            break
    starts = starts[:words[0]] if words else []
    ends = starts[1:] + [len(words)]
    return [list(words[a:b]) for a, b in zip(starts, ends)]


# ------------------------------------------------------------------ VOC (Creative Voice)

def read_voc(data):
    """-> (sample_rate, channels, bits, pcm bytes). Supports 8-bit PCM blocks (1, 2, 9)."""
    if not data.startswith(b'Creative Voice File\x1a'):
        raise FormatError('voc: bad signature')
    p = struct.unpack_from('<H', data, 20)[0]
    rate, pcm, bits, ch = 11025, bytearray(), 8, 1
    while p < len(data):
        t = data[p]
        if t == 0:
            break
        size = data[p + 1] | data[p + 2] << 8 | data[p + 3] << 16
        body = data[p + 4:p + 4 + size]
        if t == 1:
            div, codec = body[0], body[1]
            if codec != 0:
                raise FormatError('voc: compressed codec %d not supported' % codec)
            rate = 1000000 // (256 - div)
            pcm += body[2:]
        elif t == 2:
            pcm += body
        elif t == 9:
            rate, bits, ch, codec = struct.unpack_from('<IBBH', body, 0)
            if codec not in (0, 4):
                raise FormatError('voc: codec %d not supported' % codec)
            pcm += body[12:]
        p += 4 + size
    return rate, ch, bits, bytes(pcm)


def wav_bytes(rate, channels, bits, pcm):
    block = channels * bits // 8
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' +
            struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * block, block, bits) +
            b'data' + struct.pack('<I', len(pcm)) + pcm)


# ------------------------------------------------------------------ string tables

def read_strings(data, encoding='cp437'):
    """.ENG/.FRE/.GER: u16 offsets table followed by NUL-terminated strings."""
    if len(data) < 2:
        return []
    first = struct.unpack_from('<H', data, 0)[0]
    count = first // 2
    offs = struct.unpack_from('<%dH' % count, data, 0)
    out = []
    for o in offs:
        e = data.find(b'\0', o)
        out.append(data[o:e if e >= 0 else len(data)].decode(encoding, 'replace'))
    return out
