#!/usr/bin/env python3
"""tar archives (plain, gzip, bzip2, xz) via the standard tarfile module. Links are skipped."""
import os, struct, sys
sys.dont_write_bytecode = True  # keep the plugin folder clean
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "lib"))
from rtsunpack import run, Entry, log  # noqa: E402
import tarfile  # noqa: E402


def probe(path):
    try:
        return tarfile.is_tarfile(path)
    except OSError:
        return False


def entries(path):
    t = tarfile.open(path)
    for m in t:
        if not m.isfile():
            continue  # directories, symlinks, hard links, devices
        yield Entry(m.name, m.size, lambda m=m: t.extractfile(m).read())


if __name__ == '__main__':
    run('tar', '1.0.0', probe, entries)
