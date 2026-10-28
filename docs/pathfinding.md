# Pathfinding — A*, HPA*, flow fields

All pathing is deterministic integer code in `modules/rts` (no floats, ties by cell index / entity index). Costs are
10 per straight and 14 per diagonal step; a diagonal is legal only when both orthogonal detours are (no corner cutting);
passability is per path layer (`UnitType::pathLayer` = move layer + slope class on heightfields); bodies larger than
half a cell pay a clearance penalty in tight cells (A* only). Flat maps and planets (cube-sphere faces stacked in one
grid, `RtsSurface.h`) share the same rules: on planets the 8 neighbours come from the surface's neighbour table, so
face seams and cube corners (7 neighbours) are ordinary steps.

| what | where | used for |
|---|---|---|
| A* | `MapGrid::astarCells` (flat), `astarSphere` (planets) | every short path; long paths on small maps; refinement of HPA* routes |
| HPA* | `RtsHpa.cpp` (flat, maps >= 256x256), `RtsHpaSphere.cpp` (planet bodies >= 65,536 cells) | one unit's long trip: entrance graph of 16x16 clusters, then A* between consecutive waypoints |
| flow fields | `RtsFlowField.cpp` | many units to one goal: group orders >= `movement.flowField.minGroup`, map / lane waves with `waves: true` |

`MapGrid::findPath` picks A* or HPA* (string pulling on top). Flow fields are opt-in per game or map
(docs/game-format.md "Movement and bodies"); without `movement.flowField` nothing changes.

## Flow fields

- **Key and sharing.** One `FlowField` per (goal cell, goal-area radius in cells, path layer, clearance class); every unit of
  the order follows it. The goal area (all passable cells within the radius: `0.3 + spread * sqrt(units)` world units)
  is what spreads a crowd over the target instead of piling it onto one cell.
- **Integration values** = integer cost from a cell to the goal area under exactly the A* step rules (a backward
  Dijkstra: step v -> u legal iff A* could take it). Units follow the steepest strictly-decreasing neighbour, so a
  descent always ends in the goal area and never cuts a corner.
- **Small maps / bodies:** one Dijkstra over the whole grid (with the A* clearance penalty), lazily cached directions.
- **HPA* maps / bodies (hierarchical):** a resumable reverse Dijkstra over the HPA* entrance graph (the same graph and
  intra-cluster costs as HPA*, flat or planet) gives each entrance its cost D to the goal area; a cluster's values are
  a Dijkstra inside the cluster seeded with its entrances' D (and the goal cells it holds). Only clusters that units
  stand in or step into are computed, and the abstract search runs only as far as those need. Values are never below
  the A* optimum and, snapping to entrances like HPA*, a few % above it on average.
- **Invalidation.** `MapGrid::block` / `cellChanged` bump a grid stamp and the changed cluster's stamp; a field
  revalidates on its next use: seeds are recomputed, the abstract D restarts, a cluster block is rebuilt only when its
  cells changed or its seeds (entrance D) differ. Incremental results equal a full rebuild (tested).
- **Cache.** Fields are derived data (never hashed: they are pure functions of the grid). Units following a field
  reference it each tick; unreferenced fields are evicted LRU beyond `maxFields` / `budgetMB`.
- **Steering.** Every 10 ticks (staggered by entity index) and when its waypoint is reached, a unit samples the field:
  the farthest cell of the descent it can see in a straight walkable line (doubling + bisection), or the goal itself
  when close and visible; ORCA avoidance, separation and the yield rule work as for any path. A unit stuck on a field
  re-samples it first (crowds clear by themselves) and falls back to its own A* path after repeated stalls; one pushed
  into a cell that can't reach the goal falls back at once. Planet followers slide along walls on the face axes.

### Crowds (flow-field groups)

- **Arrival contagion.** A follower settles (stops, `Mobile::flowArrived` = goal cell) when it reaches the goal point,
  or when it touches a unit that already settled at the same goal while inside the settled disc
  (1.3 x 0.53 x body diameter x sqrt(settled) + 2). A follower that is stuck or has circled the crowd for 20 s settles
  wherever it touches it (up to twice the goal area); the first follower hovering 2 s in the goal area settles when the
  goal point itself is taken. The crowd fills the area around the goal from the inside out instead of jamming.
- **Pass-through.** Followers of the same field add no velocity obstacles for each other (they move the same way;
  separation keeps bodies apart) and are dropped from the ORCA candidate list before sorting; settled units are static
  obstacles for followers still arriving and never take yield requests (others go around or settle against them).
- **Cheaper avoidance:** separation and ORCA neighbour candidates come from a one-cell grid over the agents with a
  partial sort (agent-mech, bit-identical to the spatial hash); flow followers additionally drop their own crowd from
  the candidates before sorting.
- **Blocked goals** (`movement.blockedGoals: "nearest"`, rules or map; off by default because it picks different cells
  than the classic search): a path to a goal inside a structure / obstacle aims at the free cell around it nearest to
  the start, instead of A* exploring the whole reachable area for the closest cell (chasing a building).

### Measured (VM, headless `--bench-group N --bench-flow on|off`, one move order across the map; `RTS_BENCH_HIST=1`
prints where the units ended up)

See the table in the agent-flowfield report / `docs/planets.md`. On flat 256² / 1024² and a faceSize-128 planet the
order tick drops from hundreds or thousands of ms (one search per unit) to a few ms; total path time falls by 2-40x;
the remaining tick cost is local avoidance in the crowd, not pathing.
