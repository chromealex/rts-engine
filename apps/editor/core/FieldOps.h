// Grid fields in the editor (docs/fields.md): the map's initial field values live in the map JSON
// ("gridFields".<id>.values, RLE text per layer; MapDoc keeps the block verbatim in `extra`), the definitions in
// rules.json "fields" (a map may override parameters). MCP ops: field_list, field_get, field_paint,
// field_set_rules. The Fields window paints through field_paint; FieldPreview feeds the Scene view.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Workspace.h"
#include "modules/rts/RtsGridFieldData.h"
#include "modules/rts/view/RtsFieldView.h"

namespace rts::game { struct RtsScene; }

namespace rts::editor {

void registerFieldOps(Workspace& ws);

// Definitions of the open map: rules "fields" + map "gridFields" overrides (throws on invalid data).
std::vector<game::GridFieldDef> mapFieldDefs(const game::Rules& rules, const MapDoc& doc);
int fieldLayers(const game::GridFieldDef& d, const MapDoc& doc);   // 1, or player slots + 1
// Paints map cells (mx, mz) +- radius (map cells) of field `id`: mode 0 set, 1 add, 2 max, 3 erase (default).
// Bakes "fill" entries into "values". Returns cells changed; no undo snapshot (call doc.beginEdit() first).
int paintMapField(MapDoc& doc, const game::Rules& rules, const std::string& id, int mx, int mz, int radius, double value, int mode, int player,
                  std::string* err = nullptr, const int* rect = nullptr /* [x, z, w, h] map cells instead of the disc */);

// Decoded initial values for the Scene view (rebuilt when the map or rules change).
class FieldPreview {
public:
    void fill(const game::Rules& rules, const MapDoc& doc, game::RtsScene& scene);
private:
    uint64_t revision_ = ~0ull;
    const game::Rules* rules_ = nullptr;
    size_t ruleFields_ = 0;
    std::vector<game::GridFieldDef> defs_;
    std::vector<std::vector<int32_t>> values_;
    uint32_t version_ = 0;
};

} // namespace rts::editor
