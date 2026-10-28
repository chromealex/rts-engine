# Screen templates (MCP `ui_screen_create`)

Each `<template>.rml` becomes `ui/screens/<name>.rml` of a game; `$NAME` is replaced by the screen name,
`$TITLE` by the title text (a `@key` or plain text). Templates bind the data model `meta`
(engine/assets/ui/lib/meta.lua) and the shared styles `screens.rcss`. Reference: docs/ui-scripting.md.

| Template | What |
|---|---|
| blank | header with back button, empty body |
| home | main hub: currencies, Play, Shop / Profile / Settings / How to play |
| levels | map list from game.maps() with stars and locks from the profile; starts a match |
| shop | tabs: items for currencies, store products (real money), rewarded ads (ui/shop.json) |
| profile | avatar, editable name, level + XP bar, stats |
| settings | language, sound, music, how to play, reset progress |
| result | after a match (ui/screens.json "afterMatch"): victory / defeat, rewards |
| dialog | popup: title, text, OK / Cancel (ui.popup("dialog", {title, text, ok = fn})) |
