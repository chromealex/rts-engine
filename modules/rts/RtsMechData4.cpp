// agent-mech stage 4 data: morale / rout / fatigue / charge (Total War), supply lines.
#include "RtsData.h"
#include <stdexcept>

namespace rts::game {

namespace {
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error(where + ": " + what); }
uint64_t tagsOf(const Rules& r, const std::string& where, const Json& j) {
    uint64_t m = 0;
    for (auto& t : j.items()) {
        uint64_t b = r.tagMask(t.str());
        if (!b) fail(where, "unknown tag '" + t.str() + "' (no type has it)");
        m |= b;
    }
    return m;
}
} // namespace

void loadMechStage4(Rules& r, const Json& j) {
    MechRules& m = r.mech;
    if (const Json& mo = j["morale"]; mo.isObject()) {
        m.morale = m.any = true;
        m.moraleMax = mo.getFx("max", m.moraleMax);
        m.moraleRegen = mo.getFx("regen", m.moraleRegen);
        m.hitLoss = mo.getFx("hitLoss", m.hitLoss);
        m.allyDeath = mo.getFx("allyDeath", m.allyDeath);
        m.allyRadius = mo.getFx("allyRadius", m.allyRadius);
        m.flankSide = mo.getFx("flankSide", m.flankSide);
        m.flankRear = mo.getFx("flankRear", m.flankRear);
        m.waverAt = mo.getFx("waver", m.waverAt);
        m.routAt = mo.getFx("rout", m.routAt);
        m.rallyAt = mo.getFx("rally", m.rallyAt);
        m.routSpeed = mo.getFx("routSpeed", m.routSpeed);
        m.waverAttack = mo.getFx("waverAttack", m.waverAttack);
        m.moraleTags = tagsOf(r, "morale.tags", mo["tags"]);
        if (const Json& g = mo["general"]; g.isObject()) {
            m.generalTags = tagsOf(r, "morale.general.tags", g["tags"]);
            m.generalRadius = g.getFx("radius", m.generalRadius);
            m.generalRegen = g.getFx("regen", m.generalRegen);
            m.generalDeath = g.getFx("death", m.generalDeath);
        }
        if (!(m.routAt < m.waverAt) || !(m.waverAt <= m.rallyAt)) fail("morale", "needs rout < waver <= rally");
    }
    if (const Json& f = j["fatigue"]; f.isObject()) {
        m.fatigue = m.any = true;
        m.fatRun = f.getFx("run", m.fatRun);
        m.fatFight = f.getFx("fight", m.fatFight);
        m.fatRecover = f.getFx("recover", m.fatRecover);
        m.tiredAt = f.getFx("tired", m.tiredAt);
        m.exhaustedAt = f.getFx("exhausted", m.exhaustedAt);
        m.tiredMul = f.getFx("tiredMul", m.tiredMul);
        m.exhaustedMul = f.getFx("exhaustedMul", m.exhaustedMul);
        m.fatigueTags = tagsOf(r, "fatigue.tags", f["tags"]);
    }
    if (const Json& s = j["supply"]; s.isObject()) {
        m.supply = m.any = true;
        m.supplyTags = tagsOf(r, "supply.tags", s["tags"]);
        m.supplyAttrition = s.getFx("attrition", m.supplyAttrition);
        m.supplySpeed = s.getFx("speed", m.supplySpeed);
        m.supplyAttack = s.getFx("attack", m.supplyAttack);
    }
    size_t ti = 0;
    for (auto& [id, tj] : j["types"].members()) {
        UnitType& t = r.types[ti++];
        MechType& mt = t.mech;
        const std::string where = "type '" + id + "'";
        if (tj.has("morale")) { mt.moraleMax = tj.getFx("morale"); if (!m.morale) fail(where + ".morale", "needs a rules \"morale\" block"); }
        if (const Json& c = tj["charge"]; c.isObject()) {
            mt.chargeBonus = c.getFx("bonus", Fixed::fromRatio(3, 2));
            mt.chargeRun = c.getFx("run", Fixed::fromInt(3));
            mt.chargeShock = c.getFx("shock", Fixed::fromInt(15));
            mt.chargeCooldown = c.getFx("cooldown", Fixed::fromInt(8));
            m.charge = m.any = true;
        }
        if (tj.has("supplyRadius")) {
            mt.supplyRadius = tj.getFx("supplyRadius");
            if (!m.supply) fail(where + ".supplyRadius", "needs a rules \"supply\" block");
        }
    }
}

} // namespace rts::game
