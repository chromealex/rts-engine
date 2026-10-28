# Editor fonts

Fonts used only by the editor (`apps/editor/EditorTheme.cpp`); games keep `engine/assets/fonts/ui.ttf`.

| Folder | What | Version / source | License |
|---|---|---|---|
| `inter/` | Inter Regular + SemiBold: UI and heading font (Latin, Cyrillic, Greek, arrows `→ ←`, math `≈ × ≤`, `✓ • …`) | Inter 4.1, https://github.com/rsms/inter/releases/tag/v4.1 (`extras/ttf`) | SIL OFL 1.1, `inter/LICENSE.txt` |
| `noto-symbols2/` | Noto Sans Symbols 2, subset: fallback for symbols Inter lacks (`✔ ✕ ⏸ ⏹ ♦ ⚑ ...`) | notofonts/notofonts.github.io `NotoSansSymbols2-Regular.ttf`, subset with the command below | SIL OFL 1.1, `noto-symbols2/LICENSE.txt` |
| `lucide/` | Lucide icon font (all icons, private use area U+E000..), `codepoints.json` = name -> codepoint | npm `lucide-static` 1.63.0 (`font/lucide.ttf`) | ISC, `lucide/LICENSE` |

Icon constants for C++ are in `apps/editor/EditorIcons.h`; to use another Lucide icon, look its codepoint up
in `lucide/codepoints.json` (https://lucide.dev/icons) and add one line there.

Noto subset command (fontTools):

```sh
pyftsubset NotoSansSymbols2-Regular.ttf --unicodes="U+2190-21FF,U+2300-23FF,U+2460-24FF,U+25A0-25FF,U+2600-27BF,U+2900-297F,U+2B00-2BFF" \
  --layout-features='' --no-hinting --output-file=NotoSansSymbols2-editor.ttf
```
