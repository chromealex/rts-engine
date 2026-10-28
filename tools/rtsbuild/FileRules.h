// Per-file build settings (game.json "packaging.files", set from the editor's Build window):
// leave a file out, choose its zstd level (0 = store), or shrink an image for a target.
//   "packaging": {"files": [{"match": "sprites/big_*.png", "maxSize": 1024},
//                            {"match": "music/*", "level": 0}, {"match": "dev/*", "exclude": true}],
//                 "targets": {"android": {"files": [{"match": "textures/*.png", "scale": 0.5}]}}}
// Target rules come first, then the common ones; the first rule that matches decides each key.
#pragma once
#include <string>
#include <vector>
#include "engine/data/Json.h"
#include "engine/pack/PackWriter.h"

namespace rts::build {

struct FileRule {
    std::string match;
    bool exclude = false;
    int level = -1;        // -1 = keep the pack default; 0 = store
    int maxSize = 0;       // images: longest side in pixels (0 = keep)
    double scale = 0;      // images: factor (0 = keep)
};

struct FileRulesReport {
    struct Item { std::string path, what; uint64_t before = 0, after = 0; };
    std::vector<Item> items;
    std::vector<std::string> warnings;
};

std::vector<FileRule> readFileRules(const Json& gameJson, const std::string& target, std::string* error);

// Applies exclusions and image resizing to the inputs of `w` and returns the zstd level rules
// for WriteOptions::levelRules. Deterministic (same input bytes -> same output bytes).
// `fixedImages`: sprite sheets and tile atlases, whose frames are addressed in pixels; they are
// never resized (a warning says so).
bool applyFileRules(pack::PackWriter& w, const std::vector<FileRule>& rules, std::vector<std::pair<std::string, int>>& levelRules,
                    FileRulesReport& report, std::string* error, const std::vector<std::string>& fixedImages = {});

} // namespace rts::build
