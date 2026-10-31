-- View script (presentation only, floats allowed): local notices about the war.
local lost = 0

view.on("unit_died", function(e)
  if e.owner ~= view.player() or view.player() == 0 then return end
  lost = lost + 1
  if lost % 10 == 0 then view.notice(lost .. " units lost") end
end)

view.on("message", function(e)
  if e.text and (e.player == 0 or e.player == view.player()) then view.notice(e.text) end
end)

-- Script UI: a small panel (ui/hud/war.rml) fed from here; view.state() is a read-only copy of the
-- simulation scripts' `state` (scripts/rules.lua counts trained units per player there).
if view.player() ~= 0 then ui.open("war") end
ui.on("hide", function() ui.close("war") end)
view.on_frame(function()
  local me = view.player()
  if me == 0 then return end
  local trained = view.state().trained or {}
  ui.set("trained", trained[me] or 0)
  ui.set("lost", lost)
  ui.set("chest", 30 - game.tick() // game.tick_rate() % 30)
end)
