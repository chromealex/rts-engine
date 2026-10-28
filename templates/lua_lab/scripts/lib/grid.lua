-- Cell helpers shared by the lab prototypes. Cells are integer coordinates; a cell key is one integer
-- (state tables accept integer keys, iterate them in sorted order and hash them).
local grid = {}

grid.HALF = fx(1, 2)

function grid.key(cx, cz) return cx * 4096 + cz end
function grid.unkey(k) return k // 4096, k % 4096 end
function grid.cell(x, z) return fx.floor(x), fx.floor(z) end
function grid.center(cx, cz) return fx(cx) + grid.HALF, fx(cz) + grid.HALF end

function grid.inside(cx, cz)
  local w, h = game.map_size()
  return cx >= 0 and cz >= 0 and cx < w and cz < h
end

-- squared distance in fx (no sqrt needed for radius tests)
function grid.dist2(ax, az, bx, bz)
  local dx, dz = ax - bx, az - bz
  return dx * dx + dz * dz
end

return grid
