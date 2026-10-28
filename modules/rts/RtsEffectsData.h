// Parser for abilities / effects / statuses (RtsEffectsData.cpp), used by loadRules.
#pragma once
#include <functional>
#include "RtsData.h"

namespace rts::game {

struct EffectParser {
    Rules& r;
    std::function<uint16_t(const std::string&, const std::string&)> typeRef, resRef;
    std::function<uint16_t(const std::string&)> damageIdx;
    std::function<StatMod(const std::string&, const Json&)> statMod;
    std::function<uint64_t(const Json&)> tagsOf;
    std::function<int(const std::string&, const std::string&)> requirement;   // packed: tech -> 0x8000 | index

    int payload(const std::string& where, const Json& effects);   // -1 when null
    EffectDef effect(const std::string& where, const Json& j);
    int status(const std::string& where, const Json& j);           // id string or inline object
    void namedStatuses(const Json& j);
    void ability(const std::string& where, const Json& j, AbilityDef& a, bool withLevels = true);
    void playerAbility(const std::string& where, const Json& j, PlayerAbilityDef& a, bool withLevels = true);

private:
    void statusBody(const std::string& where, const Json& j, int idx);
    void filter(const std::string& where, const Json& j, EffectDef& e);
    void delivery(const std::string& where, const Json& j, DeliveryDef& d);
    int deliveredPayload(const std::string& where, const Json& aj, bool* channel);
    uint64_t statusTags(const Json& arr);
};

// Fidelity mechanics of the type / rules blocks (RtsOriginalsData.cpp), called by loadRules before the
// effect engine switch is decided.
void loadOriginals(Rules& r, const Json& j, EffectParser& ep);

} // namespace rts::game
