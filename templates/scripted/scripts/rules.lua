-- Scripted mechanics of "Iron and Gold: Scripted" (reference: docs/scripting.md).
-- Simulation script: deterministic. Integers and fx numbers only, persistent data in `state`.
local util = require("lib.util")

local WAR_CHEST_GOLD = 50
local HALL_GRACE = 60        -- seconds a player may live without a town hall

function on_start()
  state.trained = {}         -- player -> units trained
  state.without_hall = {}    -- player -> seconds without a town hall
  game.message(0, "Scripted rules: war chest every 30 s, veterans, hold your town hall!")
end

-- War chest: every 30 s the player with the most units gets gold (ties: the lower slot).
every(30, function(tick)
  local best, bestCount = nil, -1
  for _, p in ipairs(game.players()) do
    if game.player_alive(p) then
      local n = util.count_units(p)
      if n > bestCount then best, bestCount = p, n end
    end
  end
  if best then
    game.give(best, "gold", WAR_CHEST_GOLD)
    game.message(best, "War chest: +" .. WAR_CHEST_GOLD .. " gold")
  end
end)

-- Veterans: every 5th soldier a player trains comes out with +2 armor.
on("trained", function(e)
  if game.type_of(e.unit) == "peasant" then return end
  local n = (state.trained[e.owner] or 0) + 1
  state.trained[e.owner] = n
  if n % 5 == 0 then
    game.buff(e.unit, "armor", 2, 1)
    game.message(e.owner, "A veteran joins your army")
  end
end)

-- Custom defeat: a player without a town hall for HALL_GRACE seconds is out.
every(5, function(tick)
  for _, p in ipairs(game.players()) do
    if game.player_alive(p) then
      if game.count{owner = p, type = "townhall"} == 0 then
        local t = (state.without_hall[p] or 0) + 5
        state.without_hall[p] = t
        if t == 30 then game.message(p, "Rebuild your town hall within 30 seconds!") end
        if t >= HALL_GRACE then
          game.message(0, game.player(p).name .. " lost the realm")
          game.defeat(p)
        end
      else
        state.without_hall[p] = nil
      end
    end
  end
end)

-- Script commands (rules "commands"): args.units = the selected units the command accepts.
function on_command(player, id, args)
  if id == "rally_cry" then
    for _, h in ipairs(args.units) do game.buff(h, "damage", fx(1, 4), 1, 10) end
    game.message(player, "Rally cry! (" .. #args.units .. " soldiers)")
  elseif id == "muster" then
    game.order(args.units, "move", args.where.x, args.where.z)
  end
end
