-- PROTOTYPE 5: indirect control with bounty flags (Majesty), written in Lua (the core also has a
-- C++ "autonomous" + kCmdBounty version; this shows the same class of game needs no C++).
-- The player ability "bounty_flag" (cost = the reward) plants a flag; every second up to VOLUNTEERS
-- free fighters of the owner within RECRUIT cells take the job and attack-move to it. An enemy that
-- dies within the flag radius, last hit by a unit of the flag owner, pays the reward to that unit:
-- it "buys gear" (+15% damage, permanent) and the flag is done. Flags expire after LIFETIME seconds.
-- The unit_died event has no killer, so the last damage source is tracked from "damage" events.
local grid = require("lib.grid")

local VOLUNTEERS = 4
local RECRUIT = 30
local LIFETIME = 60

on("start", function()
  state.flags = {}         -- id -> {owner, x, z, r, reward, until, taken = {handle -> true}}
  state.flag_seq = 0
  state.hired = {}         -- unit handle -> flag id
  state.last_hit = {}      -- victim handle -> attacker handle
  state.bounty = {placed = 0, paid = 0, expired = 0}
end)

function place_bounty(ctx)
  state.flag_seq = state.flag_seq + 1
  state.flags[state.flag_seq] = {owner = ctx.owner, x = ctx.x, z = ctx.z, r = ctx.args.radius or 8,
                                 reward = ctx.args.reward or 50, ["until"] = game.tick() + LIFETIME * game.tick_rate()}
  state.bounty.placed = state.bounty.placed + 1
  game.message(ctx.owner, "Bounty posted: " .. (ctx.args.reward or 50) .. " gold", ctx.x, ctx.z)
end

on("damage", function(e)
  if e.source and e.unit then state.last_hit[e.unit] = e.source end
end)

on("unit_died", function(e)
  local killer = state.last_hit[e.unit]
  state.last_hit[e.unit] = nil
  state.hired[e.unit] = nil
  if not killer or not game.alive(killer) then return end
  local ko = game.owner(killer)
  for id, f in pairs(state.flags) do
    if f.owner == ko and e.owner ~= ko and grid.dist2(f.x, f.z, e.x, e.z) <= fx(f.r * f.r) then
      game.buff(killer, "damage", 0, fx("1.15"))
      game.message(ko, "A hero claimed the bounty", e.x, e.z)
      state.bounty.paid = state.bounty.paid + 1
      state.flags[id] = nil
      for h, fid in pairs(state.hired) do if fid == id then state.hired[h] = nil end end
      return
    end
  end
end)

every(1, function(tick)
  for id, f in pairs(state.flags) do
    if tick >= f["until"] then
      state.flags[id] = nil
      state.bounty.expired = state.bounty.expired + 1
      game.give(f.owner, "gold", f.reward // 2)
      for h, fid in pairs(state.hired) do if fid == id then state.hired[h] = nil end end
    else
      local n = 0
      for _, fid in pairs(state.hired) do if fid == id then n = n + 1 end end
      if n < VOLUNTEERS then
        local cands = game.units{owner = f.owner, kind = "unit", x = f.x, z = f.z, radius = RECRUIT}
        for _, h in ipairs(cands) do
          if n >= VOLUNTEERS then break end
          local t = game.type_of(h)
          if not state.hired[h] and t ~= "peasant" and t ~= "fire" then
            state.hired[h] = id
            game.order(h, "attack", f.x, f.z)
            n = n + 1
          end
        end
      end
    end
  end
end)

every(60, function()
  local b = state.bounty
  local lh = 0
  for _ in pairs(state.last_hit) do lh = lh + 1 end
  game.log("bounty: placed=" .. b.placed .. " paid=" .. b.paid .. " expired=" .. b.expired .. " tracked=" .. lh)
end)
