#!/usr/bin/env python3
"""Apple disk images. UDIF images are usually LZFSE/zlib-compressed HFS+/APFS volumes, which
the standard library can't read, so this plugin drives a system tool:
  macOS: hdiutil attach -readonly (mounted in a temp folder, always detached afterwards)
  other: 7-Zip (7zz / 7z / 7za on PATH) extracts the volume to a temp folder.
Symlinks inside the image (app bundle frameworks) are extracted as links (-snl) and skipped."""
import os
import shutil
import subprocess
import sys
import tempfile
from contextlib import contextmanager

sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry, log  # noqa: E402


def probe(path):
    try:
        with open(path, 'rb') as f:
            f.seek(-512, os.SEEK_END)
            return f.read(4) == b'koly'
    except OSError:
        return False


def seven_zip():
    for name in ('7zz', '7z', '7za'):
        p = shutil.which(name)
        if p:
            return p
    return None


@contextmanager
def opened(path):
    tmp = tempfile.mkdtemp(prefix='rts-dmg-')
    try:
        if sys.platform == 'darwin' and shutil.which('hdiutil'):
            mnt = os.path.join(tmp, 'mnt')
            os.mkdir(mnt)
            subprocess.run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-noautoopen', '-noverify',
                            '-mountpoint', mnt, path], check=True, stdout=subprocess.DEVNULL, stderr=sys.stderr)
            try:
                yield mnt
            finally:
                subprocess.run(['hdiutil', 'detach', mnt, '-force'], stdout=subprocess.DEVNULL, stderr=sys.stderr)
        else:
            sz = seven_zip()
            if not sz:
                raise RuntimeError('need hdiutil (macOS) or 7-Zip (7zz/7z) on PATH to read .dmg')
            out = os.path.join(tmp, 'x')
            subprocess.run([sz, 'x', '-y', '-snl', '-o' + out, path], check=True, stdout=subprocess.DEVNULL, stderr=sys.stderr)
            # 7-Zip puts the volume in a folder named after it; hdiutil mounts its contents directly.
            items = [x for x in os.listdir(out) if not x.startswith('[')]
            if len(items) == 1 and os.path.isdir(os.path.join(out, items[0])):
                out = os.path.join(out, items[0])
            yield out
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


HIDDEN = ('.HFS+ Private', '[HFS+ Private', '.Trashes', '.fseventsd', '.Spotlight-V100', '.journal', '\x00\x00\x00\x00HFS+ Private')


def walk(root):
    for d, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(x for x in dirs if not os.path.islink(os.path.join(d, x)))
        for name in sorted(files):
            p = os.path.join(d, name)
            if os.path.islink(p) or not os.path.isfile(p):
                continue
            yield os.path.relpath(p, root).replace(os.sep, '/'), p


def entries(path):
    with opened(path) as root:
        for rel, p in walk(root):
            if any(part.startswith(HIDDEN) for part in rel.split('/')):
                continue  # file system bookkeeping (hard-link store, trash, journal)
            yield Entry(rel, os.path.getsize(p), lambda p=p: open(p, 'rb').read())


if __name__ == '__main__':
    run('dmg', '1.0.0', probe, entries)
