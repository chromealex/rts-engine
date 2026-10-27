#!/usr/bin/env python3
"""ZIP archives via the standard zipfile module (stored, deflate, bzip2, lzma)."""
import os, struct, sys
sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry, log  # noqa: E402
import zipfile  # noqa: E402


def probe(path):
    return zipfile.is_zipfile(path)


def entries(path):
    z = zipfile.ZipFile(path)
    for info in z.infolist():
        if info.is_dir():
            continue
        yield Entry(info.filename, info.file_size, lambda info=info: z.read(info))


if __name__ == '__main__':
    run('zip', '1.0.0', probe, entries)
