-- PROTOTYPE 1: fire over a cell grid, now on grid fields (docs/fields.md). rules.json "fields": `fire` spreads
-- to neighbours with fuel (the chance grows with the fuel), burns the `fuel` field down, damages and slows
-- units standing in it and is drawn as an overlay - all C++ kernels. Ignition is data as well: the player
-- ability "ignite" and the catapult's onHit are {"type": "field", "field": "fire", "max": 100} effects.
-- The script lays the fuel (grass burns 8 steps, a tree cell 20) and fells the trees whose cell burnt out.
on("start", function()
  state.fire_stats = {trees = 0}
  local w, h = game.map_size()
  for z = 0, h - 1 do
    for x = 0, w - 1 do
      local cx, cz = field.center("fuel", x, z)
      if game.passable(cx, cz, "ground") then field.set_cell("fuel", x, z, 8) end
    end
  end
  for _, t in ipairs(game.units{type = "tree"}) do
    local x, z = game.pos(t)
    field.set("fuel", x, z, 20)
  end
end)

on_field_threshold("fuel", 1, function(e)        -- the fuel of a cell ran out: the tree there is gone
  for _, t in ipairs(game.units{type = "tree", x = e.x, z = e.z, radius = fx(3, 4)}) do
    game.kill(t, true)
    state.fire_stats.trees = state.fire_stats.trees + 1
  end
end, "fall")

every(60, function()
  local w, h = game.map_size()
  game.log("fire: burning=" .. field.count("fire", 0, 0, w, h, 1) .. " trees=" .. state.fire_stats.trees)
end)
