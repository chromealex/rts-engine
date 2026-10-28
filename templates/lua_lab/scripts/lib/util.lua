-- Small helpers shared by the scripts of this game (loaded with require("lib.util")).
local util = {}

-- The unit of `list` with the lowest hp / max_hp ratio that is hurt and not in `skip`.
function util.most_damaged(list, skip)
  local best, bestRatio = nil, nil
  for _, h in ipairs(list) do
    if not skip[h] then
      local hp, max = game.hp(h)
      if hp and hp < max then
        local ratio = hp / max            -- fx / fx = fx: no floats in simulation scripts
        if bestRatio == nil or ratio < bestRatio then best, bestRatio = h, ratio end
      end
    end
  end
  return best
end

function util.count_units(player)
  return game.count{owner = player, kind = "unit"}
end

return util
