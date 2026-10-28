-- Victory conditions (rules "victory", map overrides): who is out, who won.
-- Engine standard library: runs as the "victory" system every tick. A game replaces it with its own
-- scripts/std/victory.lua (same contract: define std.victory(tick)).
-- Mirrors the former C++ RtsModule::sysVictory exactly (bit-identical matches).

local PRES_ANY, PRES_BUILT, PRES_UNIT, PRES_KEY, PRES_STRUCTURE, PRES_DEPLOYER =
  rts.PRES_ANY, rts.PRES_BUILT, rts.PRES_UNIT, rts.PRES_KEY, rts.PRES_STRUCTURE, rts.PRES_DEPLOYER

-- does a player with these presence flags stay in the game?
local function hasBase(v, f)
  local m = v.mode
  if m == "noStructures" then return f & (PRES_BUILT | PRES_DEPLOYER) ~= 0 end   -- an MCV is a base
  if m == "noUnits" then return f & PRES_UNIT ~= 0 end
  if m == "keyUnits" then return f & (v.any_key_type and PRES_KEY or PRES_BUILT) ~= 0 end
  if m == "score" then
    if not v.key_loses then return f & PRES_ANY ~= 0 end
    return f & (v.any_key_type and PRES_KEY or PRES_STRUCTURE) ~= 0
  end
  return f & PRES_ANY ~= 0                        -- noEntities, tickets, survive, territory
end

local function aliveTeams()
  local teams, seen = {}, {}
  for _, p in ipairs(rts.players()) do
    if p.alive and not rts.is_wave_player(p.slot) and not seen[p.team] then
      seen[p.team] = true
      teams[#teams + 1] = p.team
    end
  end
  return teams
end

function std.victory(tick)
  if tick % 10 ~= 0 then return end
  if rts.winner() >= 0 then return end
  local v = rts.victory_rules()

  for _, p in ipairs(rts.players()) do
    if p.alive and not rts.is_wave_player(p.slot) then
      local has = hasBase(v, rts.presence(p.slot))
      if v.mode == "tickets" then
        local t = rts.tickets(p.team)
        if t and t <= 0 then has = false end
      end
      if v.mode == "territory" and v.territory_enabled then has = rts.territory(p.slot) > 0 end  -- out when no ground is left
      if not has then rts.defeat(p.slot) end
    end
  end

  if v.mode == "survive" then
    local teams = aliveTeams()
    if #teams == 0 then
      -- defenders fell: the wave side wins (or a draw if waves have no team of their own)
      local wp = rts.wave_players()
      rts.set_winner(#wp == 0 and 0 or rts.team_of(wp[1]))
    elseif tick >= v.limit_ticks then
      rts.set_winner(#teams == 1 and teams[1] or 0)
    end
  elseif v.mode == "territory" and rts.territory_total() > 0 then
    -- a team holding the required share of all claimable cells wins
    local cells = {}
    for _, p in ipairs(rts.players()) do
      if p.alive and not rts.is_wave_player(p.slot) then cells[p.team] = (cells[p.team] or 0) + rts.territory(p.slot) end
    end
    local need = v.share * rts.territory_total()
    for _, t in ipairs(aliveTeams()) do
      if fx(cells[t] or 0) >= need then rts.set_winner(t); break end
    end
  elseif v.mode == "score" and tick >= v.limit_ticks then
    local best, bestScore, tie = -1, fx(0), false
    for _, t in ipairs(aliveTeams()) do
      local s = rts.score(t) or fx(0)
      if best < 0 or s > bestScore then best, bestScore, tie = t, s, false
      elseif s == bestScore then tie = true end
    end
    rts.set_winner((best < 0 or tie) and 0 or best)
  end
end
