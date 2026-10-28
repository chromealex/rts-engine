-- PROTOTYPE 4: time rewind for one side (Achron-lite / "Braid RTS").
-- Every second the positions, hp and resources of every player are recorded into a ring buffer in
-- `state`; the player ability "rewind" ({"type": "script", "fn": "rewind_player", "seconds": 10})
-- brings the caster's units back to that moment:
--   * living units: hp restored (heal / damage), position via a hidden "warp" ability with a
--     teleport effect (there is no game.set_pos) - it is an order, so it lands a tick later;
--   * dead units are spawned again at their old place (new handles: buffs, levels, orders are lost);
--   * resources are given back / taken.
-- Not rewindable from Lua: projectiles, cooldowns, statuses, production queues, structures, the
-- other side (a real Achron needs world snapshots / branches in the core: Simulation::serialize exists,
-- but scripts can't reach it).
local SLOTS = 12           -- seconds kept

on("start", function()
  state.rw = {snaps = {}, head = 0, used = 0, restored = 0, respawned = 0}
end)

every(1, function(tick)
  local rw = state.rw
  rw.head = rw.head % SLOTS + 1
  local snap = {tick = tick, players = {}}
  for _, p in ipairs(game.players()) do
    local units = {}
    for _, h in ipairs(game.units{owner = p, kind = "unit"}) do
      local x, z = game.pos(h)
      local hp = game.hp(h)
      units[#units + 1] = {h = h, t = game.type_of(h), x = x, z = z, hp = hp}
    end
    snap.players[p] = {units = units, gold = game.resource(p, "gold"), wood = game.resource(p, "wood")}
  end
  rw.snaps[rw.head] = snap
end)

function rewind_player(ctx)
  local rw = state.rw
  local p = ctx.owner
  local back = ctx.args.seconds or 10
  if back >= SLOTS then back = SLOTS - 1 end
  local slot = (rw.head - 1 - back) % SLOTS + 1
  local snap = rw.snaps[slot]
  if not snap or not snap.players[p] then return end
  local old = snap.players[p]
  rw.used = rw.used + 1
  for _, u in ipairs(old.units) do
    if game.alive(u.h) then
      local hp = game.hp(u.h)
      if u.hp > hp then game.heal(u.h, u.hp - hp) elseif u.hp < hp then game.damage(u.h, hp - u.hp) end
      game.cast(u.h, "warp", u.x, u.z)
      rw.restored = rw.restored + 1
    else
      local h = game.spawn(u.t, p, u.x, u.z)
      if h then
        local hp, max = game.hp(h)
        if u.hp < max then game.damage(h, max - u.hp) end
        rw.respawned = rw.respawned + 1
      end
    end
  end
  for _, res in ipairs({"gold", "wood"}) do
    local now = game.resource(p, res)
    local was = old[res]
    if was > now then game.give(p, res, was - now) elseif was < now then game.take(p, res, now - was) end
  end
  game.message(0, game.player(p).name .. " rewinds " .. back .. " seconds")
end

every(60, function()
  local rw = state.rw
  game.log("rewind: used=" .. rw.used .. " restored=" .. rw.restored .. " respawned=" .. rw.respawned)
end)
