-- View script (presentation only): fire smoke on burning cells, notices for lab messages.
view.on("spawned", function(e)
  if e.unit_type == "fire" then view.effect("fire_smoke", e.x, e.z) end
end)

view.on("message", function(e)
  if e.text and (e.player == 0 or e.player == view.player()) then view.notice(e.text) end
end)
