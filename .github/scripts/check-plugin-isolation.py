#!/usr/bin/env python3
"""Checks that a packaged build carries only the native plugins the game selected (docs/plugins.md,
"Only what you use"). For "no plugins", every single plugin and a pair, it exports the Android project
(rtsbuild --generate-only, no SDK needed) and verifies the manifest registry, the Gradle dependencies
and repositories, the copied Java classes and plugins.cmake (plugin list, desktop mocks compiled out).

Usage: check-plugin-isolation.py <rtsbuild> <work dir>"""
import json, re, shutil, subprocess, sys
from pathlib import Path

rtsbuild, work = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
engine = Path(__file__).resolve().parents[2]
plugins = {p.parent.name: json.loads(p.read_text(encoding="utf-8")) for p in sorted((engine / "plugins").glob("*/plugin.json"))}

def android_bits(name):
    a = plugins.get(name, {}).get("android", {})
    return (set(a.get("classes", {}).values()),
            set(a.get("dependencies", [])) | set(a.get("platformDependencies", [])),
            set(a.get("repositories", [])))

errors = []
def check(cond, msg):
    if not cond:
        errors.append(msg)
        print("  FAIL:", msg)

work.mkdir(parents=True, exist_ok=True)
game = work / "game"
if game.exists():
    shutil.rmtree(game)
shutil.copytree(engine / "games" / "skirmish", game)
text = (game / "game.json").read_text(encoding="utf-8")
cfg = json.loads(re.sub(r"^\s*//.*$", "", text, flags=re.M))
cfg.pop("plugins", None)
(game / "game.json").write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")

cases = [[]] + [[n] for n in plugins] + [["applovin", "ironsource"], ["mock-ads", "admob"]]
for chosen in cases:
    label = ",".join(chosen) or "(none)"
    print(f"== {label}")
    out = work / ("out-" + ("-".join(chosen) or "none"))
    if out.exists():
        shutil.rmtree(out)
    cmd = [str(rtsbuild), "--engine", str(engine), "--project", str(game), "--target", "android", "--out", str(out), "--generate-only"]
    if chosen:
        cmd += ["--plugins", ",".join(chosen)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    proj = next((out / "android").iterdir())
    main = proj / "app" / "src" / "main"
    manifest = (main / "AndroidManifest.xml").read_text(encoding="utf-8")
    gradle = (proj / "app" / "build.gradle").read_text(encoding="utf-8")
    gradle_all = gradle + (proj / "settings.gradle").read_text(encoding="utf-8") + (proj / "build.gradle").read_text(encoding="utf-8")
    pcmake = (main / "cpp" / "plugins.cmake").read_text(encoding="utf-8")

    want_cls, want_deps, want_repos = set(), set(), set()
    for n in chosen:
        c, d, r = android_bits(n)
        want_cls |= c; want_deps |= d; want_repos |= r
    registry = set(re.findall(r'android:name="rts\.plugin\.([^"]+)"', manifest))
    check(registry == {n for n in chosen if plugins.get(n, {}).get("android", {}).get("classes")},
          f"{label}: manifest registry {sorted(registry)}")
    java = {str(p.relative_to(main / "java"))[:-5].replace("/", ".") for p in (main / "java").rglob("*.java")}
    java -= {"com.rtsengine.platform.RtsActivity", "com.rtsengine.platform.RtsPlugin"}
    check(java == want_cls, f"{label}: Java classes {sorted(java)} != {sorted(want_cls)}")
    for d in want_deps:
        check(d in gradle, f"{label}: missing dependency {d}")
    for n in plugins:
        if n in chosen:
            continue
        c, d, r = android_bits(n)
        for dep in d - want_deps:
            check(dep not in gradle, f"{label}: unused plugin {n} leaked dependency {dep}")
        for repo in r - want_repos:
            check(repo not in gradle_all, f"{label}: unused plugin {n} leaked repository {repo}")
        check(f"rts.plugin.{n}\"" not in manifest, f"{label}: unused plugin {n} in the manifest")
    m = re.search(r"set\(RTS_PLUGIN_NAMES([^)]*)\)", pcmake)
    names = set(re.findall(r'"([^"]+)"', m.group(1))) if m else None
    want_names = {n for n in chosen if n in plugins and ("android" in plugins[n] or "cpp" in plugins[n])}
    check(names == want_names, f"{label}: plugins.cmake names {names} != {want_names}")
    mocks = any(n.startswith("mock-") for n in chosen)
    check(f"set(RTS_MOCK_PLUGINS {'ON' if mocks else 'OFF'})" in pcmake, f"{label}: desktop mock plugins not compiled out")
    shutil.rmtree(out)

if errors:
    sys.exit(f"{len(errors)} isolation error(s)")
print(f"OK: {len(cases)} plugin sets, nothing unused got into a build")
