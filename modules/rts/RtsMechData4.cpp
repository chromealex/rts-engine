// agent-mech stage 4 data: morale / rout / fatigue / charge (Total War), supply lines.
#include "RtsData.h"
#include <algorithm>
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
    if (const Json& tb = j["turns"]; tb.isObject() || tb.boolean()) {
        m.turns = m.any = true;
        m.turnMoveSeconds = tb.getFx("moveSeconds", m.turnMoveSeconds);
        m.turnAttacks = int(tb.getInt("attacks", m.turnAttacks));
        m.turnTimeLimit = tb.getFx("timeLimit", m.turnTimeLimit);
        if (m.turnAttacks < 0) fail("turns.attacks", "must be >= 0");
    }
    // "flowFields": true / {"minCells", "cache"}: alias of movement.flowField {"minGroup": 1, "minCells", "maxFields"}
    // (every ground move of at least minCells cells follows a shared field); also big attack-move groups form a block
    if (const Json& ff = j["flowFields"]; ff.boolean() || ff.isObject()) {
        m.armyBlock = true;
        const int cache = int(ff.getInt("cache", 16));
        if (cache < 1 || cache > 256) fail("flowFields.cache", "1..256");
        FlowFieldRules& fr = r.movement.flow;
        if (!fr.enabled()) {   // an explicit movement.flowField wins
            fr.minGroup = 1;
            fr.minCells = int(std::clamp<int64_t>(ff.getInt("minCells", 8), 0, 100000));
            fr.maxFields = cache;
        }
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
        if (const Json& cv = tj["conveyor"]; cv.isObject()) {
            if (!t.structure || t.footprintW != 1 || t.footprintH != 1) fail(where + ".conveyor", "a conveyor piece is a 1x1 structure");
            mt.beltSpeed = cv.getFx("speed", Fixed::fromInt(2));
            if (mt.beltSpeed.raw <= 0) fail(where + ".conveyor.speed", "must be > 0");
            m.conveyors = m.any = true;
        }
        if (const Json& bi = tj["beltInput"]; !bi.isNull()) {
            if (bi.isString() && bi.str() == "any") mt.beltAccepts = ~uint64_t(0);
            else for (auto& x : bi.items()) {
                const int ri = r.findResource(x.str());
                if (ri < 0) fail(where + ".beltInput", "unknown resource '" + x.str() + "'");
                mt.beltAccepts |= uint64_t(1) << ri;
            }
            m.conveyors = m.any = true;
        }
        if (const Json& bo = tj["beltOutput"]; bo.isObject()) {
            mt.outRes = int16_t(r.findResource(bo.getStr("resource")));
            if (mt.outRes < 0) fail(where + ".beltOutput.resource", "unknown resource '" + bo.getStr("resource") + "'");
            mt.outAmount = bo.getFx("amount", Fixed::fromInt(1));
            mt.outInterval = bo.getFx("interval", Fixed::fromInt(1));
            m.conveyors = m.any = true;
        }
        if (const Json& cr = tj["craft"]; cr.isObject()) {
            auto amounts = [&](const char* key) {
                std::vector<Amount> v;
                for (auto& [res, n] : cr[key].members()) {
                    const int ri = r.findResource(res);
                    if (ri < 0) fail(where + ".craft." + key, "unknown resource '" + res + "'");
                    v.push_back({uint16_t(ri), n.fx()});
                }
                return v;
            };
            mt.craftIn = amounts("in");
            mt.craftOut = amounts("out");
            mt.craftTime = cr.getFx("time", Fixed::fromInt(2));
            if (mt.craftIn.empty() || mt.craftOut.empty()) fail(where + ".craft", "needs \"in\" and \"out\"");
            for (auto& a : mt.craftIn) mt.beltAccepts |= uint64_t(1) << a.res;
            m.conveyors = m.any = true;
        }
        if (const Json& al = tj["altitude"]; al.isObject()) {
            mt.altMin = al.getFx("min", Fixed{});
            mt.altMax = al.getFx("max", Fixed::fromInt(20));
            mt.altClimb = al.getFx("climb", Fixed::fromInt(3));
            mt.altStart = al.getFx("start", mt.altMin);
            if (!(mt.altMin < mt.altMax)) fail(where + ".altitude", "needs min < max");
            if (!(t.layer & kLayerAir)) fail(where + ".altitude", "only for air / space units (move.layer air)");
            m.altitude = m.any = true;
        }
        if (tj.has("supplyRadius")) {
            mt.supplyRadius = tj.getFx("supplyRadius");
            if (!m.supply) fail(where + ".supplyRadius", "needs a rules \"supply\" block");
        }
    }
}

} // namespace rts::game
