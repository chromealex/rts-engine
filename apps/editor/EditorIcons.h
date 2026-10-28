// Editor icons: Lucide icon font (third_party/fonts/lucide, ISC), merged into the editor's UI font by
// EditorTheme.cpp, so an icon is just a UTF-8 string: ImGui::Button(ICON_PLAY " Играть").
// Another icon: find its codepoint in third_party/fonts/lucide/codepoints.json (https://lucide.dev/icons)
// and add a line (UTF-8 bytes of the codepoint, U+E000..U+F8FF private use area).
#pragma once
#include <cstring>

#define ICON_SELECT         "\xee\x87\x83"   // mouse-pointer-2 U+E1C3
#define ICON_OBJECTS        "\xee\x8b\x90"   // boxes U+E2D0
#define ICON_CELLS          "\xee\x93\xbf"   // grid-2x2 U+E4FF
#define ICON_LEVELS         "\xee\x94\xa9"   // layers U+E529
#define ICON_HEIGHT         "\xee\x88\xb1"   // mountain U+E231
#define ICON_FIELDS         "\xee\x89\x82"   // gem U+E242
#define ICON_REGIONS        "\xee\x84\x90"   // map U+E110
#define ICON_ZONES          "\xee\x87\x8b"   // square-dashed U+E1CB
#define ICON_STARTS         "\xee\x83\x91"   // flag U+E0D1
#define ICON_ROADS          "\xee\x94\xbe"   // route U+E53E
#define ICON_ERASER         "\xee\x8a\x8f"   // eraser U+E28F
#define ICON_FIT            "\xee\x84\x92"   // maximize U+E112
#define ICON_PLAY           "\xee\x84\xbc"   // play U+E13C
#define ICON_PAUSE          "\xee\x84\xae"   // pause U+E12E
#define ICON_STOP           "\xee\x85\xa7"   // square U+E167
#define ICON_SPEED          "\xee\x86\xbf"   // gauge U+E1BF
#define ICON_FAST_FORWARD   "\xee\x82\xbd"   // fast-forward U+E0BD
#define ICON_STEP           "\xee\x8f\xaa"   // step-forward U+E3EA
#define ICON_BOT            "\xee\x86\xbb"   // bot U+E1BB
#define ICON_USER           "\xee\x86\x9f"   // user U+E19F
#define ICON_USERS          "\xee\x86\xa4"   // users U+E1A4
#define ICON_SMARTPHONE     "\xee\x85\xa3"   // smartphone U+E163
#define ICON_TABLET         "\xee\x85\xbe"   // tablet U+E17E
#define ICON_MONITOR        "\xee\x84\x9d"   // monitor U+E11D
#define ICON_ROTATE         "\xee\x85\x89"   // rotate-cw U+E149
#define ICON_LANDSCAPE      "\xee\x8d\xb6"   // rectangle-horizontal U+E376
#define ICON_PORTRAIT       "\xee\x8d\xb7"   // rectangle-vertical U+E377
#define ICON_SAFE_AREA      "\xee\x89\x97"   // scan U+E257
#define ICON_SCENE          "\xee\x8a\x9b"   // clapperboard U+E29B
#define ICON_GAME           "\xee\x83\x9f"   // gamepad-2 U+E0DF
#define ICON_HIERARCHY      "\xee\x90\x88"   // list-tree U+E408
#define ICON_INSPECTOR      "\xee\x8a\x9a"   // sliders-horizontal U+E29A
#define ICON_TOOLS          "\xee\x86\xb1"   // wrench U+E1B1
#define ICON_PROJECT        "\xee\x83\x97"   // folder U+E0D7
#define ICON_RULES          "\xee\x8d\xab"   // file-json U+E36B
#define ICON_STATS          "\xee\x8a\xa5"   // chart-line U+E2A5
#define ICON_AGENT          "\xee\x90\x92"   // sparkles U+E412
#define ICON_CONSOLE        "\xee\x86\x81"   // terminal U+E181
#define ICON_BUILD          "\xee\x83\xac"   // hammer U+E0EC
#define ICON_FX             "\xee\x83\x92"   // flame U+E0D2
#define ICON_TILES          "\xee\x83\xa9"   // grid-3x3 U+E0E9
#define ICON_TERRAIN        "\xee\x88\xb2"   // mountain-snow U+E232
#define ICON_LIGHTING       "\xee\x87\x82"   // lightbulb U+E1C2
#define ICON_FOG            "\xee\x88\x94"   // cloud-fog U+E214
#define ICON_MODELS         "\xee\x94\xa4"   // cuboid U+E524
#define ICON_SETTINGS       "\xee\x85\x94"   // settings U+E154
#define ICON_PALETTE        "\xee\x87\x9d"   // palette U+E1DD
#define ICON_SUN            "\xee\x85\xb8"   // sun U+E178
#define ICON_MOON           "\xee\x84\x9e"   // moon U+E11E
#define ICON_SAVE           "\xee\x85\x8d"   // save U+E14D
#define ICON_UNDO           "\xee\x8a\xa1"   // undo-2 U+E2A1
#define ICON_REDO           "\xee\x8a\xa0"   // redo-2 U+E2A0
#define ICON_FILE           "\xee\x83\x80"   // file U+E0C0
#define ICON_FILE_PLUS      "\xee\x83\x89"   // file-plus U+E0C9
#define ICON_FOLDER_OPEN    "\xee\x89\x87"   // folder-open U+E247
#define ICON_FOLDER_PLUS    "\xee\x83\x99"   // folder-plus U+E0D9
#define ICON_SEARCH         "\xee\x85\x91"   // search U+E151
#define ICON_CLOSE          "\xee\x86\xb2"   // x U+E1B2
#define ICON_CHECK          "\xee\x81\xac"   // check U+E06C
#define ICON_PLUS           "\xee\x84\xbd"   // plus U+E13D
#define ICON_MINUS          "\xee\x84\x9c"   // minus U+E11C
#define ICON_TRASH          "\xee\x86\x8e"   // trash-2 U+E18E
#define ICON_COPY           "\xee\x82\x9e"   // copy U+E09E
#define ICON_REFRESH        "\xee\x85\x85"   // refresh-cw U+E145
#define ICON_ALERT          "\xee\x86\x93"   // triangle-alert U+E193
#define ICON_ERROR          "\xee\x81\xb7"   // circle-alert U+E077
#define ICON_INFO           "\xee\x83\xb9"   // info U+E0F9
#define ICON_HELP           "\xee\x82\x82"   // circle-help U+E082
#define ICON_EYE            "\xee\x82\xba"   // eye U+E0BA
#define ICON_EYE_OFF        "\xee\x82\xbb"   // eye-off U+E0BB
#define ICON_ZAP            "\xee\x86\xb4"   // zap U+E1B4
#define ICON_CPU            "\xee\x82\xa9"   // cpu U+E0A9
#define ICON_HARD_DRIVE     "\xee\x83\xad"   // hard-drive U+E0ED
#define ICON_DOWNLOAD       "\xee\x82\xb2"   // download U+E0B2
#define ICON_EXTERNAL       "\xee\x82\xb9"   // external-link U+E0B9
#define ICON_LAYOUT         "\xee\x87\x81"   // layout-dashboard U+E1C1
#define ICON_WINDOW         "\xee\x90\xa6"   // app-window U+E426
#define ICON_MENU           "\xee\x84\x95"   // menu U+E115
#define ICON_MORE           "\xee\x82\xb6"   // ellipsis U+E0B6
#define ICON_CHEVRON_DOWN   "\xee\x81\xad"   // chevron-down U+E06D
#define ICON_CHEVRON_RIGHT  "\xee\x81\xaf"   // chevron-right U+E06F
#define ICON_DOT            "\xee\x91\x8f"   // dot U+E44F
#define ICON_CROSSHAIR      "\xee\x82\xac"   // crosshair U+E0AC
#define ICON_MOVE           "\xee\x84\xa1"   // move U+E121
#define ICON_BRUSH          "\xee\x8b\xa7"   // paintbrush U+E2E7
#define ICON_PIPETTE        "\xee\x84\xbb"   // pipette U+E13B
#define ICON_PENCIL         "\xee\x87\xb9"   // pencil U+E1F9
#define ICON_LINK           "\xee\x84\x82"   // link U+E102
#define ICON_SWORDS         "\xee\x8a\xb4"   // swords U+E2B4
#define ICON_SHIELD         "\xee\x85\x98"   // shield U+E158
#define ICON_CASTLE         "\xee\x8f\xa0"   // castle U+E3E0
#define ICON_TREES          "\xee\x8b\xb5"   // trees U+E2F5
#define ICON_WAVES          "\xee\x8a\x83"   // waves U+E283
#define ICON_IMAGE          "\xee\x83\xb6"   // image U+E0F6
#define ICON_TIMER          "\xee\x87\xa0"   // timer U+E1E0
#define ICON_CLOCK          "\xee\x82\x87"   // clock U+E087
#define ICON_ACTIVITY       "\xee\x80\xb8"   // activity U+E038
#define ICON_PLUG           "\xee\x8d\xbf"   // plug U+E37F
#define ICON_PLUG_ZAP       "\xee\x91\x9c"   // plug-zap U+E45C
#define ICON_GLOBE          "\xee\x83\xa8"   // globe U+E0E8
#define ICON_EXIT           "\xee\x84\x8e"   // log-out U+E10E
#define ICON_MAP_PIN        "\xee\x84\x91"   // map-pin U+E111
#define ICON_GAUGE          "\xee\x86\xbf"   // gauge U+E1BF

namespace rts::editor {

// Tab icon of a window by its stable id (EditorWindow::icon() overrides it).
inline const char* windowIconFor(const char* id) {
    static const struct { const char* id; const char* icon; } kMap[] = {
        {"scene", ICON_SCENE}, {"game", ICON_GAME}, {"hierarchy", ICON_HIERARCHY}, {"inspector", ICON_INSPECTOR},
        {"tools", ICON_TOOLS}, {"project", ICON_PROJECT}, {"rules", ICON_RULES}, {"stats", ICON_STATS},
        {"agent", ICON_AGENT}, {"console", ICON_CONSOLE}, {"build", ICON_BUILD}, {"effects", ICON_FX},
        {"tiles", ICON_TILES}, {"terrain", ICON_TERRAIN}, {"lighting", ICON_LIGHTING}, {"models", ICON_MODELS}, {"fog", ICON_FOG},
    };
    for (auto& m : kMap) if (std::strcmp(m.id, id) == 0) return m.icon;
    return ICON_WINDOW;
}

} // namespace rts::editor
