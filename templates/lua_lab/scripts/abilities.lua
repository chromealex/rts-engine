-- Scripted ability effects: {"type": "script", "fn": "<name>", ...} in rules.json.
local util = require("lib.util")

-- Mending Song (archer): heals the most damaged ally near the caster, then jumps to the most damaged
-- ally near the healed one; every jump heals 80% of the previous one.
function chain_heal(ctx)
  local args = ctx.args
  local amount = fx(args.amount)
  local radius = fx(args.jumpRadius or 5)
  local x, z = ctx.x, ctx.z
  if ctx.caster then x, z = game.pos(ctx.caster) end
  local healed = {}
  for jump = 0, args.jumps or 3 do
    local near = game.units{owner = ctx.owner, kind = "unit", x = x, z = z, radius = radius}
    local h = util.most_damaged(near, healed)
    if not h then break end
    game.heal(h, amount)
    healed[h] = true
    x, z = game.pos(h)
    amount = amount * fx(4, 5)
  end
end
