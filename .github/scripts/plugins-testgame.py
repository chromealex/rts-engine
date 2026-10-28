#!/usr/bin/env python3
"""Makes a copy of games/skirmish with every native plugin enabled (test ad ids), for CI builds that
compile the plugins against the real SDKs. Usage: plugins-testgame.py <out dir>"""
import json, re, shutil, sys
from pathlib import Path

out = Path(sys.argv[1])
if out.exists():
    shutil.rmtree(out)
shutil.copytree("games/skirmish", out)
text = (out / "game.json").read_text(encoding="utf-8")
game = json.loads(re.sub(r"^\s*//.*$", "", text, flags=re.M))
game["plugins"] = {
    "googleplay": {},
    "rustore": {"consoleAppId": "1234567", "scheme": "rtsenginetest"},
    "appstore": {},
    "admob": {
        "appId": {"android": "ca-app-pub-3940256099942544~3347511713", "ios": "ca-app-pub-3940256099942544~1458002511"},
        "interstitial": {"android": "ca-app-pub-3940256099942544/1033173712", "ios": "ca-app-pub-3940256099942544/4411468910"},
        "rewarded": {"android": "ca-app-pub-3940256099942544/5224354917", "ios": "ca-app-pub-3940256099942544/1712485313"},
        "banner": {"android": "ca-app-pub-3940256099942544/9214589741", "ios": "ca-app-pub-3940256099942544/2435281174"},
    },
    "yandex-ads": {"interstitial": "demo-interstitial-yandex", "rewarded": "demo-rewarded-yandex", "banner": "demo-banner-yandex"},
    "yandex-games": {},
    # AppLovin has no public test key: compile-only placeholders (real keys come from the dashboard)
    "applovin": {"sdkKey": "CI_PLACEHOLDER_SDK_KEY", "interstitial": "ci000000000000a1", "rewarded": "ci000000000000a2", "banner": "ci000000000000a3"},
    # LevelPlay demo app keys / ad units (developers.is.com test suite)
    "ironsource": {
        "appKey": {"android": "25b63cf85", "ios": "25c43a4a5"},
        "interstitial": {"android": "h3xw38h9214adgxo", "ios": "obg6ohwts3y690ks"},
        "rewarded": {"android": "syz3d8ekts22q0or", "ios": "l1quzz1xmmdhw5er"},
        "banner": {"android": "4fpetq4lhe5lsw3e", "ios": "xc2bsuntn9ea734t"},
    },
}
game["store"] = {"products": [{"id": "gems_100", "type": "consumable"}, {"id": "no_ads", "type": "nonConsumable"}]}
(out / "game.json").write_text(json.dumps(game, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"test game with plugins: {out}")
