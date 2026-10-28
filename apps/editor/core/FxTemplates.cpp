// Ready particle effect recipes for fx_templates (agents start from these and tune with fx_set).
// Only built-in textures (builtin:soft|spark|ring|smoke), so a template works in any game. Secondary
// layers (sparks, trails, rings) start at "minQuality": "medium" so weak phones keep the core look.
#include "FxOps.h"

namespace rts::editor {

namespace {

const FxTemplate kTemplates[] = {
{"explosion", "Fireball with flash, smoke, flying sparks with trails, ground shockwave and a light. One-shot (unit/building death, shell hit).",
R"({
  "emitters": [
    { "comment": "flash", "texture": "builtin:soft", "blend": "additive", "burst": 1, "life": 0.18, "speed": 0,
      "size": [3.2, 3.6], "sizeOverLife": [[0, 0.6], [1, 1.2]], "color": [[0, "#fff2c0ff"], [1, "#ff802000"]],
      "light": { "color": "#ffb060", "radius": 6, "intensity": 2.5, "max": 1, "alphaAffectsIntensity": true } },
    { "comment": "fireball", "texture": "builtin:smoke", "blend": "additive", "burst": 14, "life": [0.35, 0.65],
      "speed": [1, 3.5], "shape": "sphere", "radius": 0.3, "radial": 1, "drag": 3, "size": [0.9, 1.5],
      "sizeOverLife": [[0, 0.5], [1, 1.7]], "rotation": [0, 360], "spin": [-90, 90],
      "color": [[0, "#ffe6a0ff"], [0.35, "#ff7a20c0"], [1, "#40100000"]] },
    { "comment": "smoke", "texture": "builtin:smoke", "blend": "alpha", "burst": 9, "delay": 0.12, "life": [1.3, 2.2],
      "speed": [0.6, 1.6], "spread": 70, "shape": "sphere", "radius": 0.4, "gravity": [0, 0.8, 0], "drag": 1.2,
      "noise": { "strength": 0.4, "frequency": 0.6, "scroll": 0.3 }, "size": [1.0, 1.6], "sizeOverLife": [[0, 0.6], [1, 2.3]],
      "rotation": [0, 360], "spin": [-30, 30], "color": [[0, "#3a3530b0"], [0.6, "#5a554e70"], [1, "#6a666000"]] },
    { "comment": "sparks", "texture": "builtin:spark", "blend": "additive", "align": "velocity", "stretch": 0.04,
      "burst": 16, "life": [0.5, 1.0], "speed": [6, 10], "spread": 65, "gravity": [0, -12, 0], "size": [0.1, 0.18],
      "collision": { "bounce": 0.35, "dampen": 0.3, "minKillSpeed": 1 },
      "trail": { "time": 0.18, "width": [[0, 1], [1, 0]], "color": [[0, "#ffd890ff"], [1, "#ff602000"]] },
      "color": [[0, "#ffd890ff"], [1, "#ff602000"]], "minQuality": "medium" },
    { "comment": "shockwave", "texture": "builtin:ring", "blend": "additive", "align": "ground", "burst": 1, "life": 0.45,
      "speed": 0, "size": 1.2, "sizeOverLife": [[0, 0.4], [1, 4.5]], "color": [[0, "#ffe0b0a0"], [1, "#ff904000"]],
      "minQuality": "medium" }
  ]
})"},
{"fire", "Looping fire: additive flames licking upwards, rising embers, a thin smoke column and a flickering light (burning building, torch, campfire).",
R"({
  "emitters": [
    { "comment": "flames", "texture": "builtin:smoke", "blend": "additive", "rate": 26, "loop": true, "duration": 1,
      "life": [0.5, 0.9], "speed": [0.8, 1.6], "spread": 12, "shape": "cone", "radius": 0.35, "size": [0.6, 0.9],
      "sizeOverLife": [[0, 0.7], [0.4, 1], [1, 0.3]], "rotation": [0, 360], "spin": [-60, 60],
      "noise": { "strength": 0.5, "frequency": 1.2, "scroll": 1.5 },
      "color": [[0, "#ffd070ff"], [0.4, "#ff7020c0"], [1, "#60100000"]],
      "light": { "color": "#ff9040", "radius": 4, "intensity": 1.2, "ratio": 0.1, "max": 2 } },
    { "comment": "embers", "texture": "builtin:spark", "blend": "additive", "rate": 6, "loop": true, "duration": 1,
      "life": [0.8, 1.6], "speed": [1.2, 2.4], "spread": 20, "shape": "disc", "radius": 0.3, "size": [0.05, 0.09],
      "noise": { "strength": 1.2, "frequency": 1.5, "scroll": 0.8 }, "color": [[0, "#ffc060ff"], [1, "#ff402000"]],
      "minQuality": "medium" },
    { "comment": "smoke", "texture": "builtin:smoke", "blend": "alpha", "rate": 4, "loop": true, "duration": 1, "delay": 0.3,
      "life": [1.6, 2.4], "speed": [0.8, 1.2], "spread": 10, "offset": [0, 0.8, 0], "size": [0.6, 0.9],
      "sizeOverLife": [[0, 0.8], [1, 2.4]], "rotation": [0, 360], "spin": [-20, 20],
      "color": [[0, "#30302c00"], [0.2, "#30302c70"], [1, "#50504c00"]] }
  ]
})"},
{"smoke", "Looping grey smoke column drifting with noise (chimney, damaged vehicle, signal).",
R"({
  "emitters": [
    { "texture": "builtin:smoke", "blend": "alpha", "rate": 7, "loop": true, "duration": 1, "life": [2.0, 3.2],
      "speed": [0.6, 1.1], "spread": 15, "shape": "disc", "radius": 0.25, "gravity": [0.3, 0.2, 0], "drag": 0.3,
      "noise": { "strength": 0.35, "frequency": 0.5, "scroll": 0.4 }, "size": [0.7, 1.0], "sizeOverLife": [[0, 0.6], [1, 2.6]],
      "rotation": [0, 360], "spin": [-25, 25], "color": [[0, "#4a484400"], [0.15, "#4a484490"], [1, "#7a787400"]] }
  ]
})"},
{"muzzle_flash", "Very short cone of flame along the shot direction, a few sparks and a light pop (play with dir = barrel direction).",
R"({
  "emitters": [
    { "comment": "flash", "texture": "builtin:spark", "blend": "additive", "align": "velocity", "stretch": 0.08, "burst": 6,
      "life": [0.05, 0.09], "speed": [3, 6], "spread": 12, "size": [0.25, 0.4], "aspect": 1.6,
      "color": [[0, "#fff4c8ff"], [1, "#ff902000"]],
      "light": { "color": "#ffc070", "radius": 3, "intensity": 2, "max": 1 } },
    { "comment": "core", "texture": "builtin:soft", "blend": "additive", "burst": 1, "life": 0.07, "speed": 0, "size": 0.6,
      "color": [[0, "#ffffe0ff"], [1, "#ffb04000"]] },
    { "comment": "sparks", "texture": "builtin:spark", "blend": "additive", "align": "velocity", "stretch": 0.03, "burst": 4,
      "life": [0.1, 0.25], "speed": [5, 9], "spread": 25, "size": [0.04, 0.07], "gravity": [0, -6, 0],
      "color": [[0, "#ffe0a0ff"], [1, "#ff802000"]], "minQuality": "medium" }
  ]
})"},
{"magic_aura", "Looping aura around a unit: glowing ground ring, motes rising and swirling around it (local space: follows the unit).",
R"({
  "emitters": [
    { "comment": "ring", "texture": "builtin:ring", "blend": "additive", "align": "ground", "rate": 1.5, "loop": true,
      "duration": 1, "life": 1.4, "speed": 0, "size": 2.2, "space": "local", "sizeOverLife": [[0, 0.8], [1, 1.1]],
      "color": [[0, "#7fb0ff00"], [0.3, "#7fb0ffa0"], [1, "#7fb0ff00"]] },
    { "comment": "motes", "texture": "builtin:soft", "blend": "additive", "rate": 16, "loop": true, "duration": 1,
      "life": [1.0, 1.6], "speed": [0.1, 0.3], "shape": "ring", "radius": 0.9, "space": "local", "velocity": [0, 0.9, 0],
      "noise": { "strength": 0.4, "frequency": 1.5, "scroll": 0.5 }, "size": [0.1, 0.18],
      "startColor": ["#80b0ff", "#c090ff"], "color": [[0, "#ffffff00"], [0.25, "#ffffffff"], [1, "#ffffff00"]],
      "trail": { "time": 0.25, "width": [[0, 0.6], [1, 0]], "color": [[0, "#ffffffa0"], [1, "#ffffff00"]] } },
    { "comment": "glow", "texture": "builtin:soft", "blend": "additive", "rate": 2, "loop": true, "duration": 1, "life": 1.2,
      "speed": 0, "offset": [0, 0.8, 0], "size": 2.4, "space": "local", "color": [[0, "#5080ff00"], [0.5, "#5080ff30"], [1, "#5080ff00"]],
      "minQuality": "medium" }
  ]
})"},
{"heal", "Green healing burst: soft glow, rising sparkles with short trails and a ground ring (one-shot, on the healed unit).",
R"({
  "emitters": [
    { "comment": "glow", "texture": "builtin:soft", "blend": "additive", "burst": 1, "life": 0.7, "speed": 0, "offset": [0, 0.6, 0],
      "size": 2.2, "sizeOverLife": [[0, 0.5], [1, 1.2]], "color": [[0, "#70ff9000"], [0.2, "#70ff9070"], [1, "#70ff9000"]],
      "light": { "color": "#60ff80", "radius": 3, "intensity": 1, "max": 1 } },
    { "comment": "sparkles", "texture": "builtin:spark", "blend": "additive", "burst": 18, "life": [0.7, 1.2],
      "speed": [0.2, 0.6], "shape": "cone", "radius": 0.6, "spread": 10, "velocity": [0, 1.6, 0], "size": [0.12, 0.22],
      "color": [[0, "#b0ffb0ff"], [0.6, "#40ff60c0"], [1, "#20ff4000"]],
      "trail": { "time": 0.2, "width": [[0, 0.7], [1, 0]], "color": [[0, "#80ff90c0"], [1, "#40ff6000"]] } },
    { "comment": "ring", "texture": "builtin:ring", "blend": "additive", "align": "ground", "burst": 1, "life": 0.6, "speed": 0,
      "size": 0.8, "sizeOverLife": [[0, 0.5], [1, 2.4]], "color": [[0, "#80ff90c0"], [1, "#40ff6000"]], "minQuality": "medium" }
  ]
})"},
{"frost", "Ice burst: cold mist, flying ice shards and a frozen ground ring (frost nova, slow, freeze hit).",
R"({
  "emitters": [
    { "comment": "mist", "texture": "builtin:smoke", "blend": "alpha", "burst": 10, "life": [0.9, 1.5], "speed": [0.8, 1.8],
      "shape": "disc", "radius": 0.4, "spread": 80, "drag": 2, "size": [0.8, 1.3], "sizeOverLife": [[0, 0.7], [1, 1.8]],
      "rotation": [0, 360], "spin": [-20, 20], "color": [[0, "#d8f0ff00"], [0.2, "#d8f0ffa0"], [1, "#e8f8ff00"]] },
    { "comment": "shards", "texture": "builtin:spark", "blend": "additive", "align": "velocity", "stretch": 0.05, "aspect": 2,
      "burst": 14, "life": [0.4, 0.8], "speed": [4, 7], "spread": 70, "gravity": [0, -8, 0], "size": [0.1, 0.16],
      "color": [[0, "#ffffffff"], [0.5, "#a0e0ffe0"], [1, "#60b0ff00"]] },
    { "comment": "ring", "texture": "builtin:ring", "blend": "additive", "align": "ground", "burst": 1, "life": 0.8, "speed": 0,
      "size": 1, "sizeOverLife": [[0, 0.3], [0.3, 2.6], [1, 3]], "color": [[0, "#c0f0ffd0"], [1, "#80c0ff00"]],
      "light": { "color": "#90d0ff", "radius": 4, "intensity": 1.2, "max": 1 }, "minQuality": "medium" }
  ]
})"},
{"lightning", "Lightning strike: bright vertical bolts flickering down, impact flash, sparks and a strong light (sky strike, chain lightning hit).",
R"({
  "emitters": [
    { "comment": "bolts", "texture": "builtin:spark", "blend": "additive", "align": "upright", "bursts": [[0, 2], [0.06, 2], [0.12, 1]],
      "life": [0.06, 0.12], "speed": 0, "offset": [0, 3, 0], "shape": "box", "box": [0.3, 0, 0.3], "size": [0.35, 0.5],
      "aspect": 14, "color": [[0, "#ffffffff"], [1, "#a0c0ff00"]] },
    { "comment": "flash", "texture": "builtin:soft", "blend": "additive", "burst": 1, "life": 0.25, "speed": 0, "size": 3,
      "color": [[0, "#e0eaffff"], [1, "#6080ff00"]],
      "light": { "color": "#a0c0ff", "radius": 8, "intensity": 3, "max": 1 } },
    { "comment": "sparks", "texture": "builtin:spark", "blend": "additive", "align": "velocity", "stretch": 0.04, "burst": 12,
      "life": [0.2, 0.5], "speed": [4, 8], "spread": 80, "gravity": [0, -9, 0], "size": [0.05, 0.1],
      "color": [[0, "#ffffffff"], [1, "#8090ff00"]], "minQuality": "medium" }
  ]
})"},
{"blood", "Blood hit: dark red droplets flying out, falling and stopping on the ground (unit hit / death).",
R"({
  "emitters": [
    { "comment": "droplets", "texture": "builtin:soft", "blend": "alpha", "align": "velocity", "stretch": 0.03, "burst": 16,
      "life": [0.5, 0.9], "speed": [2, 4.5], "spread": 55, "gravity": [0, -14, 0], "size": [0.07, 0.14],
      "collision": { "bounce": 0, "dampen": 1, "lifeLoss": 0.2 }, "color": [[0, "#8a0a08ff"], [0.8, "#5a0504ff"], [1, "#40030200"]] },
    { "comment": "mist", "texture": "builtin:smoke", "blend": "alpha", "burst": 3, "life": [0.25, 0.4], "speed": [0.5, 1],
      "size": [0.4, 0.6], "sizeOverLife": [[0, 0.6], [1, 1.5]], "color": [[0, "#7a080690"], [1, "#50040300"]] }
  ]
})"},
{"dust_trail", "Dust kicked up behind a moving unit (attach and move it, or bind as a projectile trail): emits per metre moved.",
R"({
  "emitters": [
    { "texture": "builtin:smoke", "blend": "alpha", "perMeter": 3, "loop": true, "life": [0.8, 1.4], "speed": [0.2, 0.6],
      "spread": 60, "shape": "disc", "radius": 0.3, "gravity": [0, 0.3, 0], "drag": 1.5, "size": [0.4, 0.7],
      "sizeOverLife": [[0, 0.6], [1, 2]], "rotation": [0, 360], "spin": [-30, 30],
      "color": [[0, "#a08a6800"], [0.15, "#a08a6890"], [1, "#b09a7800"]] }
  ]
})"},
{"rain", "Looping rain over a 16x16 area: falling streaks and small splash rings on the ground (play it at the camera target or map centre).",
R"({
  "emitters": [
    { "comment": "drops", "texture": "builtin:spark", "blend": "additive", "align": "velocity", "stretch": 0.02, "aspect": 2,
      "rate": 220, "loop": true, "duration": 1, "life": 0.9, "speed": [14, 16], "direction": [0.1, -1, 0], "spread": 2,
      "shape": "box", "box": [16, 0, 16], "offset": [0, 12, 0], "size": [0.03, 0.04],
      "collision": { "bounce": 0, "dampen": 1, "minKillSpeed": 100 }, "color": "#a0b8d080" },
    { "comment": "splashes", "texture": "builtin:ring", "blend": "additive", "align": "ground", "rate": 60, "loop": true,
      "duration": 1, "life": 0.25, "speed": 0, "shape": "box", "box": [16, 0, 16], "offset": [0, 0.02, 0], "size": 0.25,
      "sizeOverLife": [[0, 0.3], [1, 1]], "color": [[0, "#c0d8f060"], [1, "#c0d8f000"]], "minQuality": "medium" }
  ]
})"},
{"snow", "Looping snowfall over a 16x16 area: soft flakes drifting down with noise.",
R"({
  "emitters": [
    { "texture": "builtin:soft", "blend": "alpha", "rate": 70, "loop": true, "duration": 1, "life": [6, 8], "speed": [0.8, 1.2],
      "direction": [0, -1, 0], "spread": 10, "shape": "box", "box": [16, 0, 16], "offset": [0, 8, 0], "size": [0.06, 0.12],
      "noise": { "strength": 0.6, "frequency": 0.4, "scroll": 0.3 }, "rotation": [0, 360],
      "color": [[0, "#ffffff00"], [0.1, "#ffffffe0"], [0.85, "#ffffffe0"], [1, "#ffffff00"]] }
  ]
})"},
{"portal", "Looping portal: a spinning vertical ring of light, particles drawn into the centre, ground glow and a light.",
R"({
  "emitters": [
    { "comment": "ring", "texture": "builtin:ring", "blend": "additive", "align": "upright", "rate": 3, "loop": true, "duration": 1,
      "life": 1.2, "speed": 0, "offset": [0, 1.3, 0], "size": 2.4, "spin": 90, "sizeOverLife": [[0, 0.9], [1, 1.05]],
      "color": [[0, "#b060ff00"], [0.4, "#b060ffb0"], [1, "#6030ff00"]],
      "light": { "color": "#a060ff", "radius": 5, "intensity": 1.5, "max": 1 } },
    { "comment": "inflow", "texture": "builtin:spark", "blend": "additive", "rate": 40, "loop": true, "duration": 1, "life": 0.8,
      "speed": [1.2, 1.6], "shape": "cone", "radius": 1.3, "thickness": 0, "spread": 90, "radial": -1, "direction": [0, 0, -1],
      "offset": [0, 1.3, 0], "space": "local", "size": [0.06, 0.1],
      "color": [[0, "#e0c0ff00"], [0.3, "#e0c0ffff"], [1, "#8040ff00"]] },
    { "comment": "core", "texture": "builtin:soft", "blend": "additive", "rate": 4, "loop": true, "duration": 1, "life": 0.8,
      "speed": 0, "offset": [0, 1.3, 0], "size": [1.2, 1.6], "color": [[0, "#9050ff00"], [0.5, "#9050ff60"], [1, "#9050ff00"]] },
    { "comment": "ground", "texture": "builtin:soft", "blend": "additive", "align": "ground", "rate": 1, "loop": true, "duration": 1,
      "life": 1.5, "speed": 0, "size": 3, "color": [[0, "#8040ff00"], [0.5, "#8040ff50"], [1, "#8040ff00"]], "minQuality": "medium" }
  ]
})"},
{"level_up", "Level up: golden column of rising sparks with trails, a ground ring burst and a short light.",
R"({
  "emitters": [
    { "comment": "column", "texture": "builtin:spark", "blend": "additive", "rate": 50, "duration": 0.6, "life": [0.7, 1.1],
      "speed": [0.1, 0.3], "shape": "ring", "radius": 0.7, "velocity": [0, 2.6, 0], "size": [0.1, 0.2],
      "color": [[0, "#fff0a0ff"], [0.6, "#ffc040d0"], [1, "#ff902000"]],
      "trail": { "time": 0.3, "width": [[0, 0.8], [1, 0]], "color": [[0, "#ffe080c0"], [1, "#ffa02000"]] } },
    { "comment": "ring", "texture": "builtin:ring", "blend": "additive", "align": "ground", "burst": 1, "life": 0.7, "speed": 0,
      "size": 1, "sizeOverLife": [[0, 0.4], [1, 3.2]], "color": [[0, "#ffe090e0"], [1, "#ffa03000"]],
      "light": { "color": "#ffd070", "radius": 4, "intensity": 1.5, "max": 1 } },
    { "comment": "glow", "texture": "builtin:soft", "blend": "additive", "burst": 1, "life": 0.9, "speed": 0, "offset": [0, 1, 0],
      "size": 2.5, "aspect": 1.8, "color": [[0, "#ffd06000"], [0.3, "#ffd06060"], [1, "#ffd06000"]], "minQuality": "medium" }
  ]
})"},
};

} // namespace

const FxTemplate* fxTemplates(size_t& count) {
    count = sizeof(kTemplates) / sizeof(kTemplates[0]);
    return kTemplates;
}

} // namespace rts::editor
