Lua 5.4.7 (https://www.lua.org, sources from https://github.com/lua/lua tag v5.4.7), MIT license (LICENSE).
Vendored: the core and standard libraries (lua.c / luac.c / onelua.c / ltests.* left out). Unmodified.
CMake target rts_lua (CMakeLists.txt, "Lua" block). The engine-wide shared Lua state will live in the engine;
the UI attaches RmlUi documents to it (engine/ui/UiLua.h).
