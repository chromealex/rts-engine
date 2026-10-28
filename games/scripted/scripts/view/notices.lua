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
