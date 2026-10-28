"""Helper for RTSEngine unpacker plugins (protocol 1). Python 3.8+, standard library only.

A plugin implements two functions and calls run():

    def probe(path) -> bool             # cheap content check
    def entries(path) -> iterable of Entry(path, size, read)   # read() -> bytes

run() handles the protocol: argv = [method, request.json], one JSON response line on stdout,
errors as {"error": {"message": ...}} with exit code 1, logs on stderr. extract() writes files
itself and never lets an archive path escape the output folder.
"""
import json
import os
import sys
import traceback
from collections import namedtuple

PROTOCOL = 1

Entry = namedtuple('Entry', 'path size read')


def log(*args):
    print(*args, file=sys.stderr)


def safe_path(name):
    """Archive path -> safe relative path ('a/b.txt'); None if nothing usable remains."""
    parts = []
    for p in name.replace('\\', '/').split('/'):
        if p in ('', '.', '..') or ':' in p:
            continue  # drop absolute roots, drive letters, parent references
        parts.append(''.join(c for c in p if c >= ' ' and c not in '<>"|?*'))
    parts = [p for p in parts if p.strip(' .')]
    return '/'.join(parts) or None


def _respond(obj, code=0):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + '\n')
    sys.stdout.flush()
    sys.exit(code)


class _Namer:
    """Unique, collision-free output names. Handles names that differ only in case (macOS and
    Windows disks are case-insensitive) and file/folder clashes ('a' as a file, then 'a/b')."""

    def __init__(self):
        self.files, self.dirs, self.dir_map = set(), set(), {}

    @staticmethod
    def _bump(name, taken):
        base, ext = os.path.splitext(name)
        i = 2
        while (base + '~%d' % i + ext).lower() in taken:
            i += 1
        return base + '~%d' % i + ext

    def name(self, path):
        parts = path.split('/')
        out = []
        for i, part in enumerate(parts[:-1]):  # folders: reuse earlier decisions
            key = '/'.join(parts[:i + 1]).lower()
            if key not in self.dir_map:
                cand = '/'.join(out + [part])
                if cand.lower() in self.files:
                    cand = '/'.join(out + [self._bump(part, self.files | self.dirs)])
                self.dir_map[key] = cand.split('/')[-1]
                self.dirs.add(cand.lower())
            out.append(self.dir_map[key])
        cand = '/'.join(out + [parts[-1]])
        if cand.lower() in self.files or cand.lower() in self.dirs:
            cand = '/'.join(out + [self._bump(parts[-1], self.files | self.dirs)])
        self.files.add(cand.lower())
        return cand


def run(name, version, probe, entries, capabilities=('probe', 'list', 'extract')):
    try:
        if len(sys.argv) < 2:
            _respond({'error': {'message': 'usage: <method> <request.json>'}}, 2)
        method = sys.argv[1]
        req = {}
        if len(sys.argv) > 2:
            with open(sys.argv[2], 'r', encoding='utf-8') as f:
                req = json.load(f)
        if req.get('protocol', PROTOCOL) != PROTOCOL:
            _respond({'error': {'message': 'protocol %s not supported' % req.get('protocol')}}, 1)
        if method == 'info':
            _respond({'protocol': PROTOCOL, 'name': name, 'version': version, 'capabilities': list(capabilities)})
        archive = req.get('archive')
        if not archive:
            _respond({'error': {'message': 'request has no "archive"'}}, 1)
        if method == 'probe':
            _respond({'match': bool(probe(archive))})
        if method == 'list':
            out, namer = [], _Namer()
            for e in entries(archive):
                p = safe_path(e.path)
                if p:
                    out.append({'path': namer.name(p), 'size': int(e.size)})
            _respond({'entries': out})
        if method == 'extract':
            root = os.path.abspath(req['out'])
            os.makedirs(root, exist_ok=True)
            n, total, namer = 0, 0, _Namer()
            for e in entries(archive):
                p = safe_path(e.path)
                if not p:
                    continue
                p = namer.name(p)
                dst = os.path.abspath(os.path.join(root, p))
                if not dst.startswith(root + os.sep):
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                data = e.read()
                with open(dst, 'wb') as f:
                    f.write(data)
                n += 1
                total += len(data)
            _respond({'files': n, 'bytes': total})
        _respond({'error': {'message': 'unknown method ' + method}}, 1)
    except SystemExit:
        raise
    except Exception as ex:  # noqa: BLE001 - every failure must become a protocol error
        traceback.print_exc(file=sys.stderr)
        _respond({'error': {'message': '%s: %s' % (type(ex).__name__, ex)}}, 1)
