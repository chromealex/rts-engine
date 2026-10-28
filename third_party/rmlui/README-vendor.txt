RmlUi 6.1 (https://github.com/mikke89/RmlUi), MIT license (LICENSE.txt).
Vendored: Include/, Source/Core (without FontEngineDefault/FreeType), Source/Debugger.
The font engine is provided by engine/ui (stb_truetype).
Also vendored: Source/Lua + Include/RmlUi/Lua (the Lua plugin), built as rts_rmlui_lua against third_party/lua.
Local change: Source/Lua/Interpreter.cpp loads scripts in text mode only (luaL_loadbufferx(..., "t")):
precompiled Lua bytecode can't be checked and may crash the VM, so game content can't ship it.
