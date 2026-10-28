# engine/i18n — localization

Presentation-only. Nothing here may feed the simulation: translated text is never hashed, compared
or stored in sim state, and the client localizes its own display copy of `Rules`.

## Files

| File | What |
|---|---|
| `Localization.{h,cpp}` | string tables, fallback chain, `{placeholders}`, CLDR plural rules, pseudo-locale, `check()` |
| `GameLanguage.{h,cpp}` | `game.json` languages, choosing the language (saved → OS → source), loading engine + game tables |
| `SystemLanguage.cpp` | OS languages: CoreFoundation (macOS/iOS), `GetUserPreferredUILanguages` (Windows), `navigator.languages` (web), system properties (Android), `LANGUAGE`/`LANG` (Linux). `RTS_LANG=de` overrides everywhere |
| `modules/rts/RtsLocalize.{h,cpp}` | key scheme for RTS data, `localizeRules()`, `collectSourceStrings()` |
| `tools/i18n` | `rts_i18n check` (tables and `.rml` documents) / `rts_i18n extract` |
| `engine/assets/lang/<tag>.json` | engine/client strings (`en` is the base) |
| `<project>/lang/<tag>.json` | a game's strings; may also override engine keys |

## Table format

```jsonc
{
  "@language": "Русский",                 // name of the language in itself (language menu)
  "menu": { "map": "Карта" },             // nested objects flatten to "menu.map"
  "notice.built": "Построено: {name}",    // dotted keys work too
  "hud": {
    "wave_in": { "one": "Волна через {count} секунду", "few": "… секунды", "many": "… секунд", "other": "… секунды" }
  }
}
```

- Placeholders: `{name}`; `{{` / `}}` for literal braces. Unknown placeholders stay visible.
- Plurals: an object whose keys are only `zero/one/two/few/many/other` (with `other`). The form is
  chosen by the `count` argument (or the first numeric argument). `requiredPlurals(lang)` lists the
  forms a language needs (ru/uk: one, few, many, other; en: one, other; ja/zh: other).
- Lookup chain: `pt-BR` → `pt` → game source language → `en` → caller's fallback → the key itself.

## Using it in code

```cpp
i18n::Localization loc;
i18n::loadGameLocalization(loc, reader, i18n::readGameLanguages(manifest), tag);
loc.tr("notice.built", i18n::Args().str("name", type.name));
loc.tr("hud.wave_in", i18n::Args().num("count", seconds));
loc.trOr("type.footman", "Footman");          // data text as the fallback
loc.text("@campaign.intro");                  // "@key" in data → lookup
```

In RmlUi documents (`engine/assets/ui/*.rml`, or a game's `ui/*.rml` override), static text uses
the `tr` transform of the client's data model:

```html
<h2>{{ 'menu.map' | tr(lang) }}</h2>
```

`lang` is only there so the text is re-evaluated when the player switches language (no document
reload). Text with values (`Health {hp} / {max}`) is formatted in C++ and bound as a variable.
`rts_i18n check` reports unknown keys and hard-coded words in documents.

Every user-facing string goes through `tr()`; add the key to `engine/assets/lang/en.json` **and**
`ru.json` in the same change (`rts_tests` fails if `ru` misses a key or a placeholder differs).
Never build sentences by concatenation (`"Built: " + name`) — word order differs between languages;
put the whole sentence in the table with placeholders.

## Game data keys

Names in `rules.json`/maps stay in the game's `sourceLanguage` and need no change. Translations live
under derived keys: `game.name`, `map.<stem>.title|name|player.<n>`, `resource.<id>`, `type.<id>`,
`tech.<id>`, `faction.<id>`, `ability.<id>`. A name written as `"@my.key"` uses that key instead.

```sh
rts_i18n extract --project templates/skirmish --lang de --write   # creates/updates lang/de.json, // TODO marks new keys
rts_i18n check   --project templates/skirmish                     # errors: bad placeholders/plurals; warnings: untranslated
```

## Testing

- `--lang ru` on the client command line, or `RTS_LANG=ru`.
- `--lang qps`: pseudo-locale — every string accented and ~35% longer in `[ ]`. Text that shows up
  plain is hard-coded; text that gets clipped needs a wider layout.

## Fonts

The default UI font (Noto Sans) covers Latin, Cyrillic and Greek. CJK, Arabic, Hebrew, Thai, Hindi
need their own font: a table can name one with `"@font"` (`loc.meta("@font")`); loading it into
the UI is up to the UI layer. Arabic/Hebrew also need RTL shaping, which ImGui does not do.
