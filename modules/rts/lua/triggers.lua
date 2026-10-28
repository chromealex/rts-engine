-- Triggers (rules / map "triggers": event -> conditions -> actions), docs/game-format.md § Triggers.
-- Engine standard library: the "triggers" system (std.triggers), last C++ system of the tick so it
-- sees every event. Parsing stays in C++ (RtsTriggers.cpp); the runtime state (fired / edge / inside
-- flags, variables, messages) lives in GameState::trig, so it is hashed and rolled back like before.
-- Unit scans (counts, targets, area membership) are C++ helpers; this file decides what fires and
-- what happens. Mirrors the former C++ RtsModule::sysTriggers exactly (bit-identical matches).

local DEFS = rts.trigger_defs()
local AREAS = rts.trigger_areas()
local TYPES = rts.type_flags()
local HOSTILE = rts.HOSTILE_NEUTRAL

local function compare(a, cmp, b)
  if cmp == ">=" then return a >= b end
  if cmp == ">" then return a > b end
  if cmp == "<=" then return a <= b end
  if cmp == "<" then return a < b end
  if cmp == "==" then return a == b end
  return not (a == b)
end

local function filterMatches(f, t)       -- t: type index
  if f.type and f.type ~= t then return false end
  if f.tags ~= 0 and TYPES.tags[t + 1] & f.tags == 0 then return false end
  if f.structures and not TYPES.structure[t + 1] then return false end
  if f.units and TYPES.structure[t + 1] then return false end
  return true
end

local function areaContains(a, x, z)
  return x >= a.x and z >= a.z and x < a.x + a.w and z < a.z + a.h
end

local function tickRate() return game.tick_rate() end
local function secondsToTicks(s) return fx.int(s * tickRate()) end

-- ---------------------------------------------------------------- conditions
local function conditionsHold(di, d, player)
  for ci, c in ipairs(d.conditions) do
    local ok = false
    local k = c.kind
    if k == "count" then
      ok = compare(fx(rts.trig_count(di, ci, player)), c.cmp, c.value)
    elseif k == "resource" then
      ok = true
      local slots = rts.player_slots()
      for _, p in ipairs(rts.trig_players(di, "c", ci, player)) do
        if p < slots and not compare(rts.stock(p, c.index), c.cmp, c.value) then ok = false end
      end
    elseif k == "variable" then
      ok = compare(rts.var(c.name), c.cmp, c.value)
    elseif k == "researched" then
      ok = true
      local slots = rts.player_slots()
      for _, p in ipairs(rts.trig_players(di, "c", ci, player)) do
        if p >= slots or not rts.researched(p, c.index) then ok = false end
      end
    elseif k == "random" then
      ok = rts.rng_unit() < c.value
    elseif k == "time" then
      ok = compare(rts.ratio(game.tick(), tickRate()), c.cmp, c.value)
    elseif k == "alive" then
      ok = true
      for _, p in ipairs(rts.trig_players(di, "c", ci, player)) do
        if not rts.alive(p) then ok = false end
      end
    elseif k == "regions" then
      ok = compare(fx(rts.regions_owned(player)), c.cmp, c.value)
    elseif k == "script" then
      ok = rts.trig_script_condition(di, ci, player)
    end
    if ok == c.negate then return false end
  end
  return true
end

-- ---------------------------------------------------------------- actions
local SEVEN_TENTHS = rts.ratio(7, 10)

local function runActions(di, d, player, unit, wx, wy, wz)
  for ai, a in ipairs(d.actions) do
    local players = rts.trig_players(di, "a", ai, player)
    local function targets() return rts.trig_targets(di, ai, player, unit) end
    -- where: explicit point, area centre, the unit, else the event's position
    local ax, ay, az = wx, wy, wz
    if a.has_at then ax, ay, az = a.at_x, fx(0), a.at_z
    elseif a.area >= 0 then
      local ar = AREAS[a.area + 1]
      ax, ay, az = ar.x + ar.w / 2, fx(0), ar.z + ar.h / 2
    elseif unit and rts.alive_entity(unit) then
      local x, y, z = rts.pos(unit)
      if x then ax, ay, az = x, y, z end
    end
    local k = a.kind
    if k == "spawn" then
      if a.type then
        local owner = players[1] or 0
        local made = {}
        local structure = TYPES.structure[a.type + 1]
        for n = 0, a.count - 1 do
          local px = ax + fx(n % 4) * SEVEN_TENTHS
          local pz = az + fx(n // 4) * SEVEN_TENTHS
          local e
          if structure then e = rts.spawn(a.type, owner, px, ay, pz)
          else e = rts.spawn_on_cell(a.type, owner, px, ay, pz, 6) end
          if e then made[#made + 1] = e end
        end
        if a.attack_move then
          for _, n in ipairs(made) do
            if rts.has_orders(n) then
              local nx, ny, nz = rts.pos(n)
              local bx, _, bz = rts.nearest_enemy_structure(owner, nx, ny, nz)
              if bx then rts.order(n, rts.ORDER_ATTACK_MOVE, bx, bz, 4) end
            end
          end
        end
      end
    elseif k == "give" then
      for _, p in ipairs(players) do rts.add_resource(p, a.index, a.amount) end
    elseif k == "take" then
      local slots = rts.player_slots()
      for _, p in ipairs(players) do
        if p < slots then rts.stock(p, a.index, fx.max(fx(0), rts.stock(p, a.index) - a.amount)) end
      end
    elseif k == "setOwner" then
      local to = players[1] or player
      local ts = a.triggering_unit and targets() or rts.trig_owner_targets(di, ai)
      for _, e in ipairs(ts) do rts.set_owner(e, to) end
    elseif k == "kill" then
      for _, e in ipairs(targets()) do rts.hp(e, 0) end
    elseif k == "remove" then
      for _, e in ipairs(targets()) do rts.hp(e, 0); rts.add_unit_flags(e, rts.UNIT_SILENT) end
    elseif k == "damage" then
      for _, e in ipairs(targets()) do rts.deal_damage(e, a.amount, 0, nil, 0) end
    elseif k == "heal" then
      for _, e in ipairs(targets()) do rts.hp(e, fx.min(rts.stat(e, rts.STAT_MAX_HP), rts.hp(e) + a.amount)) end
    elseif k == "buff" then
      local untilTick = a.duration > 0 and game.tick() + secondsToTicks(a.duration) or 0xFFFFFFFF
      for _, e in ipairs(targets()) do rts.add_buff(e, a.stat, a.add, a.mul, untilTick) end
    elseif k == "message" then
      local idx = rts.message_index(a.text)
      if a.player.rel == 1 or #players == 0 then rts.emit(rts.EV_MESSAGE, 0, unit, nil, idx, ax, ay, az)
      else for _, p in ipairs(players) do rts.emit(rts.EV_MESSAGE, p, unit, nil, idx, ax, ay, az) end end
    elseif k == "setVariable" then
      rts.var(a.name, a.amount)
    elseif k == "addVariable" then
      rts.add_var(a.name, a.amount)
    elseif k == "research" then
      for _, p in ipairs(players) do rts.research(p, a.index) end
    elseif k == "enableType" or k == "disableType" then
      if a.type then for _, p in ipairs(players) do rts.set_type_disabled(p, a.type, k == "disableType") end end
    elseif k == "victory" then
      if players[1] then rts.set_winner(rts.team_of(players[1])) end
    elseif k == "defeat" then
      for _, p in ipairs(players) do if rts.alive(p) then rts.defeat(p, 0) end end
    elseif k == "enableTrigger" or k == "disableTrigger" then
      if a.trigger >= 0 then rts.trig_enabled(a.trigger + 1, k == "enableTrigger") end
    elseif k == "addScore" then
      for _, p in ipairs(players) do rts.add_score(rts.team_of(p), a.amount) end
    elseif k == "setRegionOwner" then
      rts.set_region_owner_by_name(a.name, players[1] or 0)
    elseif k == "attackMove" or k == "move" then
      local kind = k == "attackMove" and rts.ORDER_ATTACK_MOVE or rts.ORDER_MOVE
      for _, e in ipairs(targets()) do
        if rts.has_orders(e) then rts.order(e, kind, ax, az, 8) end
      end
    elseif k == "setTickets" then
      for _, p in ipairs(players) do rts.set_tickets(rts.team_of(p), a.amount) end
    elseif k == "castAbility" then
      for _, p in ipairs(players) do rts.cast_player_ability(p, a.index, ax, ay, az, unit, a.attack_move) end
    elseif k == "script" then
      rts.trig_script_action(di, ai, player, unit, ax, ay, az)
    end
  end
end

-- ---------------------------------------------------------------- firing
local function fire(di, player, unit, x, y, z)
  local d = DEFS[di]
  if not rts.trig_enabled(di) then return end
  if d.once and rts.trig_fired(di, player) then return end
  if not conditionsHold(di, d, player) then return end
  if d.once then rts.trig_fired(di, player, true) end
  runActions(di, d, player, unit, x, y, z)
end

local ZERO = fx(0)
local function fireGlobal(di)
  if DEFS[di].for_each_player then
    for _, p in ipairs(rts.players()) do fire(di, p.slot, nil, ZERO, ZERO, ZERO) end
  else
    fire(di, 0, nil, ZERO, ZERO, ZERO)
  end
end

local EVENT_KIND = {
  [rts.EV_DIED] = "unitDied", [rts.EV_BUILD_DONE] = "built", [rts.EV_TRAIN_DONE] = "trained", [rts.EV_RESEARCHED] = "researched",
  [rts.EV_CAPTURED] = "captured", [rts.EV_WAVE] = "wave", [rts.EV_REGION_CLAIMED] = "regionClaimed",
  [rts.EV_PLAYER_DEFEATED] = "playerDefeated", [rts.EV_ABILITY] = "ability", [rts.EV_LEVEL_UP] = "levelUp",
}
local UNIT_EVENT = { unitDied = true, built = true, trained = true }
local TIMED = { start = true, time = true, every = true }
local EDGE = { enterArea = true, leaveArea = true, resource = true, variable = true }

-- what this game's triggers need, decided once (constant data)
local hasTimed, hasEdge, eventMask = false, false, 0
for _, d in ipairs(DEFS) do
  if TIMED[d.event] then hasTimed = true end
  if EDGE[d.event] then hasEdge = true end
  for ty, kind in pairs(EVENT_KIND) do
    if d.event == kind then eventMask = eventMask | (1 << ty) end
  end
end
std.period.triggers = #DEFS > 0 and 1 or 0

function std.triggers(tick)
  rts.trig_init()
  local rate = tickRate()

  -- time-based
  if hasTimed then
    local started = rts.trig_started()
    for di, d in ipairs(DEFS) do
      if d.event == "start" and not started then fireGlobal(di) end
      if d.event == "time" and tick == fx.int(d.seconds * rate) then fireGlobal(di) end
      if d.event == "every" then
        local period = math.max(1, fx.int(d.seconds * rate))
        if tick > 0 and tick % period == 0 then fireGlobal(di) end
      end
    end
  end
  rts.trig_started(true)

  -- simulation events of this tick (including the ones the actions above emitted)
  if eventMask ~= 0 then
    for _, ev in ipairs(rts.trig_events(eventMask)) do
      local kind = EVENT_KIND[ev.type]
      for di, d in ipairs(DEFS) do
        if d.event == kind and rts.trig_player_matches(di, ev.player, ev.player) then
          local skip = false
          if UNIT_EVENT[kind] and (ev.value < 0 or ev.value >= #TYPES.structure or not filterMatches(d.filter, ev.value)) then skip = true end
          if not skip and kind == "researched" and d.index >= 0 and ev.value ~= d.index then skip = true end
          if not skip and d.area >= 0 and not areaContains(AREAS[d.area + 1], ev.x, ev.z) then skip = true end
          if not skip then fire(di, ev.player == HOSTILE and 0 or ev.player, ev.a, ev.x, ev.y, ev.z) end
        end
      end
    end
  end

  -- area enter / leave and edge-triggered thresholds (every 5 ticks)
  if not hasEdge or tick % 5 ~= 0 then return end
  for di, d in ipairs(DEFS) do
    if d.event == "enterArea" then
      for _, en in ipairs(rts.trig_area_update(di)) do
        local e, owner = en[1], en[2]
        local x, y, z = rts.pos(e)
        fire(di, owner == HOSTILE and 0 or owner, e, x, y, z)
      end
    elseif d.event == "leaveArea" then
      for _, en in ipairs(rts.trig_area_update(di)) do
        local e = en[1]
        if rts.alive_entity(e) then
          local owner = rts.owner(e)
          local x, y, z = rts.pos(e)
          fire(di, owner == HOSTILE and 0 or owner, e, x, y, z)
        end
      end
    elseif d.event == "resource" then
      local slots = rts.player_slots()
      for _, p in ipairs(rts.players()) do
        if p.slot < slots and rts.trig_player_matches(di, p.slot, p.slot) then
          local v = compare(rts.stock(p.slot, d.index), d.cmp, d.value)
          if v and not rts.trig_edge(di, p.slot) then fire(di, p.slot, nil, ZERO, ZERO, ZERO) end
          rts.trig_edge(di, p.slot, v)
        end
      end
    elseif d.event == "variable" then
      local v = compare(rts.var(d.name), d.cmp, d.value)
      if v and not rts.trig_edge(di, 0) then fireGlobal(di) end
      rts.trig_edge(di, 0, v)
    end
  end
end
