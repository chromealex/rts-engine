-- PROTOTYPE 3: programmable units (Gladiabots / Screeps-lite).
-- Each unit type of player PLAYER runs a small rule program (first matching rule wins), evaluated every
-- second. An action that does not apply (already home, no target) falls through to the next rule. Programs are plain data, so a UI could edit them - but the UI cannot reach the simulation
-- with arbitrary data (no script commands), so here they are constants.
-- Conflict: the built-in AI of the same player keeps issuing orders too (no API to take a unit or a
-- player away from the C++ AI); the program wins only because it re-issues every second.
local grid = require("lib.grid")

local PLAYER = 1
local PROGRAMS = {
  archer = {
    {when = "hp_below", value = 35, act = "retreat"},
    {when = "enemy_within", value = 7, act = "focus_weakest"},
  },
  footman = {
    {when = "hp_below", value = 20, act = "retreat"},
    {when = "enemy_within", value = 5, act = "focus_weakest"},
  },
  catapult = {
    {when = "enemy_unit_within", value = 3, act = "retreat"},
  },
}

on("start", function()
  state.prog = {fired = {}}
end)

local function home(p, x, z)
  return game.nearest(x, z, {owner = p, type = "townhall"}) or game.nearest(x, z, {owner = p, kind = "structure"})
end

local function enemies_near(p, x, z, r, kind)
  local out = {}
  for _, h in ipairs(game.units{x = x, z = z, radius = r, kind = kind}) do
    local o = game.owner(h)
    if o and o ~= 0 and game.hostile(p, o) then out[#out + 1] = h end
  end
  return out
end

local CONDITIONS = {
  hp_below = function(h, v, x, z, p) local hp, max = game.hp(h); return hp * 100 < max * v end,
  enemy_within = function(h, v, x, z, p) return #enemies_near(p, x, z, v) > 0 end,
  enemy_unit_within = function(h, v, x, z, p) return #enemies_near(p, x, z, v, "unit") > 0 end,
}

local ACTIONS = {
  retreat = function(h, x, z, p)
    local b = home(p, x, z)
    if not b then return false end
    local bx, bz = game.pos(b)
    if grid.dist2(bx, bz, x, z) < fx(36) then return false end   -- already home: next rule
    game.order(h, "move", bx, bz)
    return true
  end,
  focus_weakest = function(h, x, z, p)
    local best, bestHp = nil, nil
    for _, e in ipairs(enemies_near(p, x, z, 8)) do
      local hp = game.hp(e)
      if bestHp == nil or hp < bestHp then best, bestHp = e, hp end
    end
    if not best then return false end
    game.order(h, "attack", best)
    return true
  end,
}

every(1, function()
  if not game.player_alive(PLAYER) then return end
  local fired = state.prog.fired
  for _, h in ipairs(game.units{owner = PLAYER, kind = "unit"}) do
    local prog = PROGRAMS[game.type_of(h)]
    if prog then
      local x, z = game.pos(h)
      for i, rule in ipairs(prog) do
        if CONDITIONS[rule.when](h, rule.value, x, z, PLAYER) and ACTIONS[rule.act](h, x, z, PLAYER) then
          fired[rule.act] = (fired[rule.act] or 0) + 1
          break
        end
      end
    end
  end
end)

every(60, function()
  local s = ""
  for k, v in pairs(state.prog.fired) do s = s .. " " .. k .. "=" .. v end
  game.log("programs:" .. s)
end)
