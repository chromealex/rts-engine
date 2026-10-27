# Unpacker protocol, version 1

Unpackers add support for archive formats the engine doesn't know: zip, tar, disk images, and
the data files of other games. An unpacker is a plugin folder with a manifest and a program
written in any language. Host tools (`assetpipe`, `rtsbuild`, the editor/MCP server) call it to
turn an archive into plain files, which are then packed into `.rpak`. **The game runtime never
runs unpackers**: it reads `.rpak` only, the same on every platform.

To add a new format, drop a folder into one of the search paths. The engine doesn't need to be
rebuilt.

## Search path

Later entries win on name clashes:

1. `<engine>/tools/unpackers/*/unpacker.json`: plugins shipped with the engine.
2. Every folder listed in `$RTS_UNPACKERS` (`:`-separated, `;` on Windows): the user's own
   plugins, e.g. importers for commercial games that are kept outside the repo.
3. `<project>/unpackers/*/unpacker.json`: plugins that belong to one game (never packed into
   the game's content).
4. `--unpackers <dir>` on the `assetpipe` command line.

## Manifest: `unpacker.json`

```json
{
  "protocol": 1,
  "name": "war",
  "title": "Warcraft I / II data archive",
  "version": "1.0.0",
  "extensions": [".war", ".sud"],
  "magic": [{"offset": 0, "hex": "19000000"}, {"offset": -512, "ascii": "koly"}],
  "capabilities": ["probe", "list", "extract"],
  "external": true,
  "license": "MIT",
  "command": ["python3", "unpack.py"],
  "command_windows": ["py", "-3", "unpack.py"]
}
```

| field | meaning |
|---|---|
| `protocol` | Must be `1`. Manifests with any other value are rejected with a readable error. |
| `name` | Unique id, `[A-Za-z0-9_-]`. `rpak` is reserved for the built-in format. |
| `extensions` | Case-insensitive file suffixes. Multi-part suffixes like `.tar.gz` are allowed. |
| `magic` | Byte signatures, given as `hex` or `ascii`. A negative `offset` counts from the end of the file. |
| `capabilities` | Any of `probe`, `list`, `extract`. |
| `external` | Defaults to `true`: the format holds third-party data. Packs built from it get the *external* flag and must not be committed or redistributed. Set `false` for neutral containers whose contents the user owns. |
| `command` | Program and its arguments. Arguments that name a file inside the plugin folder are made absolute, and `${dir}` expands to the plugin folder. On Windows, `command_windows` replaces it when present. |

A manifest needs at least one of `extensions` or `magic`, so the format can be detected.

## Detection

Candidates are ranked: magic and extension both match, then magic only, then extension only.
Each candidate is confirmed by its `probe`, and the first one that answers `match: true` wins.
Because of the probe step, weak signatures and extensions shared by unrelated formats (`.pac`,
`.dat`) can't misfire. `--format <name>` on the command line, or `"format"` in game.json, skips
detection.

## Calls

Each call is a separate process:

```
<command...> <method> <request.json>
```

- **Request:** a JSON object in the file named on the command line. It always contains
  `"protocol": 1`.
- **Response:** one JSON object on stdout. If stdout has several lines, the last non-empty
  line is taken as the response, so stray prints from libraries don't break the call.
- **Logs:** stderr, which is passed through to the user.
- **Errors:** `{"error": {"message": "..."}}` with a non-zero exit code.

| method | request | response |
|---|---|---|
| `info` | `{}` | `{"protocol": 1, "name": "...", "version": "...", "capabilities": [...]}` |
| `probe` | `{"archive": "/abs/path"}` | `{"match": true}` |
| `list` | `{"archive": "/abs/path"}` | `{"entries": [{"path": "a/b.bin", "size": 123}]}` |
| `extract` | `{"archive": "/abs/path", "out": "/abs/empty/dir"}` | `{"files": 12, "bytes": 3456}` |

`extract` writes files under `out`, keeping their archive paths. Rules:

- Use `/` as the separator.
- No absolute paths, no `..`.
- Names that collide case-insensitively get a `~2` suffix, because the macOS and Windows disks
  that developers use are case-insensitive.
- When the archive has no names (offset tables), number the entries `NNNN.<ext>`, guessing the
  extension from the content (`.wav` for RIFF, `.xmi`, `.bin`).
- Keep the bytes as they are. Converting images, sprites or maps into engine formats is a
  separate importer step, not the unpacker's job.

## What the host guarantees

- The plugin's output is untrusted. After `extract` the host scans `out` itself, ignores the
  paths the plugin reported, and never follows symlinks.
- The scratch folder is created fresh for each call and removed afterwards, even on failure.
- A plugin can't break the build silently. A bad manifest, a missing program, wrong JSON, a
  non-zero exit code or a protocol mismatch each become a readable error that names the plugin
  and the method.

## Writing a plugin

```sh
assetpipe unpackers new <dir> <name>          # manifest + unpack.py + rtsunpack.py helper
assetpipe --unpackers <dir> unpackers check   # manifest valid + `info` handshake
assetpipe --unpackers <dir> probe some.file   # detection
assetpipe --unpackers <dir> list some.file    # what is inside
assetpipe --unpackers <dir> import -o x.rpak some.file=imported/x
```

With the Python helper (`lib/rtsunpack.py`, standard library only, Python 3.8+), a plugin
consists of two functions:

```python
def probe(path): ...                  # cheap content check -> bool
def entries(path): yield Entry(name, size, read)   # read() -> bytes
run('name', '1.0.0', probe, entries)
```

`run()` implements the whole protocol: request parsing, safe output paths, name collisions and
error reporting.

## Machine interface (MCP server, editor)

Every `assetpipe` command accepts `--json` and then prints exactly one JSON object on stdout:
`{"ok": true, ...}` or `{"ok": false, "error": "..."}`. These are the operations to expose:

| operation | command |
|---|---|
| Known formats | `assetpipe --json formats` |
| Detect the format of a file | `assetpipe --json probe <file>` |
| List what is inside | `assetpipe --json list <archive>` |
| Import into a pack | `assetpipe --json import -o <out.rpak> <src>[=<prefix>]... [--format F] [--include glob] [--external]` |
| Create a plugin | `assetpipe --json unpackers new <dir> <name>` |
| Check a plugin | `assetpipe --json unpackers check [<name or dir>...]` |

Adding a new format from the MCP server takes four steps:

1. Run `unpackers new` to create the plugin.
2. Edit `unpack.py`.
3. Run `unpackers check` to validate it.
4. Run `import`.

## Imports in game.json

`rtsbuild` runs the imports listed in game.json while building `content.rpak`:

```json
"imports": [
  {"source": "~/Games/Warcraft2/war2/DATA/MAINDAT.WAR", "prefix": "imported/war2/main", "optional": true},
  {"source": "${HOME}/Downloads/zdemo/ZDemoPart3.zip", "prefix": "imported/z", "include": ["AUDIO/*"]}
]
```

| field | meaning |
|---|---|
| `source` | A file or folder. Relative paths are resolved against the game folder. `~` and `${VAR}` are expanded. |
| `prefix` | Where the files land inside the pack. |
| `format` | Plugin name; skips detection. |
| `include` | Glob filters on archive paths. |
| `optional` | If the source is missing, skip it with a note instead of failing. Use it for third-party data that isn't on every machine. |
| `external` | Force the external flag, for folders of third-party files. |

The game reads imported files like any other content:
`platform::readContentFile("imported/war2/main/0001.bin")`.

## Bundled plugins

| name | formats | notes |
|---|---|---|
| `zip` | `.zip`, `.oramap`, `.pk3`, `.pk4` | Python `zipfile`. |
| `tar` | `.tar`, `.tar.gz`, `.tgz`, `.tar.bz2`, `.tar.xz` | Python `tarfile`. Links are skipped. |
| `dmg` | `.dmg` | macOS: `hdiutil attach -readonly`. Elsewhere: 7-Zip (`7zz`/`7z`). With 7-Zip, HFS+ hard links come out empty. |
| `war` | Warcraft I/II `.WAR`, `.SUD`, `.INS` | Stored and LZ entries. 1-byte placeholder entries are skipped. |
| `zpac` | Z (Bitmap Brothers) `.PAC` | Single LZSS stream. Checked against the demo: `DMAIN.PAC` starts with `PHRASES.BIN` byte for byte. |
| `wwpak` | Westwood `.PAK` (Dune II, Lands of Lore, Kyrandia 1–2) | Header of (offset, 8.3 name) records. Checked on all 15 Dune II PAKs (665 files): `.CPS` sizes match their own headers, and `DUNEMAP.CPS` decodes into the Arrakis map. |
| `zrsc` | Z `.RSC` | Offset table of sprite records, written as raw records. |
