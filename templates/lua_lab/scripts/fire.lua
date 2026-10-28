-- PROTOTYPE 1: fire spreading over a cell grid (Wildfire / Ostriv / "RTS with fire").
-- The core has no script-defined grid fields and no ground decals, so:
--   * the field lives in `state.fire` (sparse: only burning cells), the fuel map in `state.fuel`;
--   * a burning cell is shown by a neutral, invulnerable "fire" marker unit (rules.json types.fire);
--   * trees are fuel: a burning tree cell burns longer and the tree is destroyed.
-- Sources: player ability "ignite" ({"type": "script", "fn": "ignite_at"}) and catapult hits (15%).
local grid = require("lib.grid")

local STEP = 10            -- ticks between updates (0.5 s at 20 ticks/s)
local BURN = 8             -- steps a grass cell burns (4 s)
local BURN_TREE = 20       -- steps a tree cell burns (10 s)
local SPREAD = 18          -- % chance per neighbour per step
local SPREAD_TREE = 45     -- % chance into a tree cell
local DAMAGE = 9           -- hp per step to every unit standing in a burning cell
local MAX_CELLS = 300      -- safety cap: a forest fire must not eat the instruction budget
local NEIGH = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}}

on("start", function()
  state.fire = {}          -- key -> {left = steps, e = marker handle}
  state.burnt = {}         -- key -> true (ash: never burns again)
  state.fuel = {}          -- key -> tree handle
  state.fire_stats = {ignited = 0, cells = 0, damage = 0, trees = 0, peak = 0}
  for _, h in ipairs(game.units{type = "tree"}) do
    local x, z = game.pos(h)
    local cx, cz = grid.cell(x, z)
    state.fuel[grid.key(cx, cz)] = h
  end
end)

local function burning_count()
  local n = 0
  for _ in pairs(state.fire) do n = n + 1 end
  return n
end

-- Starts a fire in one cell; returns true when it caught.
local function ignite_cell(cx, cz)
  if not grid.inside(cx, cz) then return false end
  local k = grid.key(cx, cz)
  if state.fire[k] or state.burnt[k] then return false end
  local x, z = grid.center(cx, cz)
  local tree = state.fuel[k]
  if not tree and not game.passable(x, z, "ground") then return false end   -- water / rock: no fuel
  local marker = game.spawn("fire", 0, x, z)
  state.fire[k] = {left = tree and BURN_TREE or BURN, e = marker or 0}
  state.fire_stats.cells = state.fire_stats.cells + 1
  return true
end

-- ability effect: {"type": "script", "fn": "ignite_at"} (player ability "ignite")
function ignite_at(ctx)
  local cx, cz = grid.cell(ctx.x, ctx.z)
  local n = 0
  for dx = -1, 1 do
    for dz = -1, 1 do
      if ignite_cell(cx + dx, cz + dz) then n = n + 1 end
    end
  end
  if n > 0 then
    state.fire_stats.ignited = state.fire_stats.ignited + 1
    game.message(0, "Fire! (" .. n .. " cells)", ctx.x, ctx.z)
  end
end

-- catapult stones set the ground on fire (a data hook would need a weapon "onHit" script effect;
-- the damage event is enough here)
on("damage", function(e)
  if not e.source or game.type_of(e.source) ~= "catapult" then return end
  if game.random(100) >= 15 then return end
  local cx, cz = grid.cell(e.x, e.z)
  if ignite_cell(cx, cz) then state.fire_stats.ignited = state.fire_stats.ignited + 1 end
end)

on("tick", function(tick)
  if tick % STEP ~= 0 then return end
  local fire = state.fire
  local count = burning_count()
  local spread = {}
  local done = {}
  for k, f in pairs(fire) do                     -- sorted keys: deterministic
    local cx, cz = grid.unkey(k)
    local x, z = grid.center(cx, cz)
    for _, h in ipairs(game.units{x = x, z = z, radius = fx(3, 4), kind = "unit"}) do
      local t = game.type_of(h)
      if t ~= "fire" then
        game.damage(h, DAMAGE)
        state.fire_stats.damage = state.fire_stats.damage + DAMAGE
      end
    end
    for _, d in ipairs(NEIGH) do
      local nx, nz = cx + d[1], cz + d[2]
      local nk = grid.key(nx, nz)
      if not fire[nk] and not state.burnt[nk] then
        local chance = state.fuel[nk] and SPREAD_TREE or SPREAD
        if game.random(100) < chance then spread[#spread + 1] = {nx, nz} end
      end
    end
    f.left = f.left - 1
    if f.left <= 0 then done[#done + 1] = k end
  end
  for _, k in ipairs(done) do
    local f = fire[k]
    if f.e ~= 0 then game.kill(f.e, true) end
    local tree = state.fuel[k]
    if tree then
      game.kill(tree, true)
      state.fuel[k] = nil
      state.fire_stats.trees = state.fire_stats.trees + 1
    end
    fire[k] = nil
    state.burnt[k] = true
  end
  for _, c in ipairs(spread) do
    if count >= MAX_CELLS then break end
    if ignite_cell(c[1], c[2]) then count = count + 1 end
  end
  if count > state.fire_stats.peak then state.fire_stats.peak = count end
end)

every(60, function()
  local s = state.fire_stats
  game.log("fire: ignitions=" .. s.ignited .. " cells=" .. s.cells .. " peak=" .. s.peak ..
           " damage=" .. s.damage .. " trees=" .. s.trees)
end)
