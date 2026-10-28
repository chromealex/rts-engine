-- Bench handlers. Map triggers set bench_n (units) and bench_mode:
--   0 idle   spawn only (baseline)
--   1 read   every tick: game.pos + game.hp of every unit, fx sum
--   2 unit   every tick: game.unit(h) table of every unit
--   3 state  every tick: per-unit record {x, z, hp} written into `state` (serialized + hashed per tick)
--   4 order  every second: every unit gets a move order to a random point (C++ pathing included)
--   5 field  every tick: a 64x64 integer grid in `state` updated with a diffusion step (fire / fluid)
--   6 data   every tick: the same per-unit record as `state`, kept in game.data(h) (per-entity, G3)
local FIELD = 64

on("start", function()
  local n = fx.int(game.var("bench_n"))
  state.mode = fx.int(game.var("bench_mode"))
  local w, h = game.map_size()
  local side = 1
  while side * side < n do side = side + 1 end
  local x0 = (w - side) // 2
  local spawned = 0
  for i = 0, n - 1 do
    local x, z = x0 + i % side, x0 + i // side
    if game.spawn("dummy", 0, fx(x) + fx(1, 2), fx(z) + fx(1, 2)) then spawned = spawned + 1 end
  end
  state.u = {}
  state.sum = fx(0)
  if state.mode == 5 then
    state.f = {}
    for i = 1, FIELD * FIELD do state.f[i] = (i * 7919) % 1000 end
  end
  game.log("bench: n=" .. n .. " spawned=" .. spawned .. " mode=" .. state.mode)
end)

on("tick", function(tick)
  local mode = state.mode
  if mode == 1 then
    local s = fx(0)
    for _, h in ipairs(game.units{kind = "unit"}) do
      local x, z = game.pos(h)
      local hp = game.hp(h)
      s = s + x + z + hp
    end
    state.sum = s
  elseif mode == 2 then
    local c = 0
    for _, h in ipairs(game.units{kind = "unit"}) do
      local u = game.unit(h)
      if u.hp > 50 then c = c + 1 end
    end
    state.sum = fx(c)
  elseif mode == 3 then
    local u = {}
    for _, h in ipairs(game.units{kind = "unit"}) do
      local x, z = game.pos(h)
      u[h] = {x = x, z = z, hp = game.hp(h)}
    end
    state.u = u
  elseif mode == 6 then
    for _, h in ipairs(game.units{kind = "unit"}) do
      local x, z = game.pos(h)
      local d = game.data(h)
      d.x, d.z, d.hp = x, z, game.hp(h)
    end
  elseif mode == 4 then
    if tick % 20 == 0 then
      local w, hh = game.map_size()
      for _, h in ipairs(game.units{kind = "unit", owner = 0}) do
        game.order(h, "move", game.random(8, w - 8), game.random(8, hh - 8))
      end
    end
  elseif mode == 5 then
    local f = state.f
    local g = {}
    for i = 1, FIELD * FIELD do
      local v = f[i] * 4
      local l, r, up, dn = f[i - 1] or 0, f[i + 1] or 0, f[i - FIELD] or 0, f[i + FIELD] or 0
      g[i] = (v + l + r + up + dn) // 8
    end
    state.f = g
  end
end)
