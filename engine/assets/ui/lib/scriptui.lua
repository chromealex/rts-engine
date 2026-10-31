-- In-match UI driven by the game's view scripts (docs/scripting.md § Script UI). The client runs it after
-- meta.lua. View scripts call ui.set / ui.open / ui.close (their own Lua state); the client forwards
-- them here as scriptui._set / _open / _close. Documents are the game's ui/hud/<name>.rml with
-- <div data-model="script">: {{key}} for ui.set(key, ...), {{params.x}} for ui.open(name, params),
-- data-event-click="event('name', ...)" -> ui.on('name', fn(args)) in the view scripts,
-- data-event-click="send('command_id', {...})" -> a rules "commands" command (like view.send).
-- Don't edit per game; games style their documents.

local ctx = rmlui.contexts["main"]

scriptui = {}
local docs, wanted = {}, {}
local visible = false   -- documents show while the match is on screen (playing)

local model = ctx:OpenDataModel("script", {
    params = {},
    event = function(ev, name, ...) scripthost.event(tostring(name), {...}) end,
    send = function(ev, id, args) scripthost.send(tostring(id), type(args) == "table" and args or {}) end,
})

function scriptui._set(key, value) model[key] = value end

function scriptui._open(name, params)
    local d = docs[name]
    if not d then
        model.params = params or {}
        d = ctx:LoadDocument("engine/ui/hud/" .. name .. ".rml")   -- the game's ui/hud/<name>.rml
        if not d then print("scriptui: no document ui/hud/" .. tostring(name) .. ".rml"); return end
        docs[name] = d
    elseif params then
        model.params = params
    end
    wanted[name] = true
    if visible then d:Show() end
end

function scriptui._close(name)
    wanted[name] = nil
    if docs[name] then docs[name]:Hide() end
end

-- a new match: forget the previous game's documents and values
function scriptui._reset()
    for _, d in pairs(docs) do d:Close() end
    docs, wanted = {}, {}
end

game.on("screen", function(s)
    visible = s == "playing"
    for name, d in pairs(docs) do
        if visible and wanted[name] then d:Show() else d:Hide() end
    end
end)
