#!/usr/bin/env python3
"""Prints a game's packaging info as KEY=value lines (append to $GITHUB_OUTPUT in CI).

Usage: game-info.py templates/skirmish
Mirrors tools/rtsbuild: game.json may contain comments; EXE is rtsbuild's sanitize(name).
"""
import json, re, sys
from pathlib import Path


def strip_comments(text):
    out, i, n, in_str = [], 0, len(text), False
    while i < n:
        c = text[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(text[i + 1]); i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True; out.append(c)
        elif text.startswith("//", i):
            while i < n and text[i] != "\n":
                i += 1
            continue
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        else:
            out.append(c)
        i += 1
    return "".join(out)


def sanitize(name):  # same as rtsbuild: every non-alphanumeric *byte* becomes '_', runs collapse
    b = name.encode("utf-8")
    s = "".join(chr(x) if chr(x).isascii() and chr(x).isalnum() else "_" for x in b)
    s = re.sub("_+", "_", s)
    return s or "Game"


game = json.loads(strip_comments((Path(sys.argv[1]) / "game.json").read_text(encoding="utf-8")))
name = game.get("name", "Game")
info = {
    "NAME": name,
    "ID": game.get("id", "com.example.game"),
    "VERSION": game.get("version", "0.1.0"),
    "BUILD": str(game.get("build", 1)),
    "EXE": sanitize(name),
}
for k, v in info.items():
    print(f"{k}={v}")
