-- Waves: scripted map waves (map "waves": They Are Billions, tower defense, survival) and lane waves
-- (types with "wave" spawners on the shared rules "waveClock"); everything a wave spawns attack-moves
-- on the nearest hostile key structure / structure / unit.
-- Engine standard library: the "waves" system (std.waves). Mirrors the former C++ sysWaves exactly.

local WAVES = rts.waves()          -- static definitions (constant for the match); progress lives in C++ (wave_done)
local CLOCK = rts.wave_clock_ticks()

std.period.waves = (#WAVES > 0 or CLOCK >= 0) and 1 or 0   -- not called at all without waves

local function march(spawned)
  for _, n in ipairs(spawned) do
    if rts.has_orders(n) then
      local owner = rts.owner(n)
      local px, py, pz = rts.pos(n)
      local gx, _, gz = rts.wave_objective(owner, px, py, pz)
      if gx then rts.order(n, rts.ORDER_ATTACK_MOVE, gx, gz, 4) end
    end
  end
end

function std.waves(tick)
  local spawned = {}
  for i, wd in ipairs(WAVES) do
    local done = rts.wave_done(i)
    if not (wd.repeats >= 0 and done > wd.repeats) then
      local due = wd.tick + done * wd.every
      if tick == due and not (done > 0 and wd.every == 0) then
        local scale = fx(1)
        for _ = 1, done do scale = scale * wd.growth end
        for _, s in ipairs(wd.spawn) do
          local n = fx.int(scale * s.count)
          for _ = 1, n do
            local ox = (rts.rng_unit() * 2 - 1) * wd.spread
            local oz = (rts.rng_unit() * 2 - 1) * wd.spread
            local e = rts.spawn_on_cell(s.type, wd.player, wd.x + ox, wd.z + oz, 6)
            if e then spawned[#spawned + 1] = e end
          end
        end
        rts.emit(rts.EV_WAVE, wd.player, nil, nil, done, wd.x, wd.y, wd.z)
        done = done + 1
        if wd.every == 0 then done = wd.repeats + 1 end
        rts.wave_done(i, done)
      end
    end
  end
  -- lane waves: every structure with a wave spawner releases its squad on the shared clock
  local nextTick = rts.next_wave_tick()
  if CLOCK >= 0 and tick >= nextTick and nextTick > 0 then
    local rel = rts.lane_wave_sources()
    for _, r in ipairs(rel) do
      for _ = 1, r.count do
        local x, y, z = rts.free_spot(r.from, r.type)
        local n = rts.spawn(r.type, r.owner, x, y, z)
        spawned[#spawned + 1] = n
        rts.emit(rts.EV_TRAIN_DONE, r.owner, n, r.from, r.type, x, y, z)
      end
    end
    rts.next_wave_tick(tick + CLOCK)
    rts.emit(rts.EV_WAVE, 0, nil, nil, #rel)
  end
  march(spawned)
end
