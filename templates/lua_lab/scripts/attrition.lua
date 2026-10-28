-- PROTOTYPE 2: attrition outside own territory (Rise of Nations).
-- Territory = within RADIUS of a finished own or allied structure. A unit deep in enemy territory
-- (near an enemy structure and far from any friendly one) loses PERCENT of its max hp every PERIOD.
-- Units with the tag "supply" (none in this game: data hook) would stop attrition around them.
-- The C++ core has a territory layer (rules "territory"), but no script access to it and no attrition;
-- here territory is recomputed from structures in Lua every period (cost O(units x structures)).
local grid = require("lib.grid")

local PERIOD = 2           -- seconds
local RADIUS = 9           -- cells of territory around a structure
local PERCENT = 3          -- % of max hp per period

on("start", function()
  state.attrition = {hits = 0, damage = fx(0), killed = 0}
end)

local function structures_by_team()
  local by = {}
  for _, h in ipairs(game.units{kind = "structure"}) do
    local u = game.unit(h)
    if u.owner >= 1 and u.owner < 255 and not u.under_construction then
      local t = game.team(u.owner)
      local list = by[t]
      if not list then list = {}; by[t] = list end
      list[#list + 1] = {u.x, u.z}
    end
  end
  return by
end

local function near_any(list, x, z, r2)
  if not list then return false end
  for _, p in ipairs(list) do
    if grid.dist2(p[1], p[2], x, z) <= r2 then return true end
  end
  return false
end

every(PERIOD, function()
  local by = structures_by_team()
  local r2 = fx(RADIUS * RADIUS)
  for _, p in ipairs(game.players()) do
    if game.player_alive(p) then
      local team = game.team(p)
      for _, h in ipairs(game.units{owner = p, kind = "unit"}) do
        local x, z = game.pos(h)
        if not near_any(by[team], x, z, r2) then
          local enemy = false
          for t, list in pairs(by) do
            if t ~= team and near_any(list, x, z, r2) then enemy = true; break end
          end
          if enemy then
            local hp, max = game.hp(h)
            local loss = fx.max(fx(1), max * PERCENT / 100)
            game.damage(h, loss)
            local a = state.attrition
            a.hits = a.hits + 1
            a.damage = a.damage + loss
            if loss >= hp then a.killed = a.killed + 1 end
          end
        end
      end
    end
  end
end)

every(60, function()
  local a = state.attrition
  game.log("attrition: hits=" .. a.hits .. " damage=" .. tostring(a.damage) .. " killed=" .. a.killed)
end)
