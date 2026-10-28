// agent-mech: combat and economy models found missing by the templates audit.
// - armorModel: "subtract" (default, StarCraft: damage - armor, at least minFraction / min),
//   "percent" (Warcraft III: armor a reduces damage by a*f / (1 + a*f); negative armor amplifies),
//   "classes" (Age of Empires II: weapons have class attacks, types class armors; damage = max(min, sum over
//   classes of max(0, attack - armor)); base classes always count, bonus classes only against types that have them).
// - shields (StarCraft): absorb damage first (minus shield armor), regenerate after a delay without hits.
// - upkeepResources: upkeep cuts gathering of these resources only (Warcraft III: gold).
// - resourceNode.maxWorkers: at most N workers gather a node at once; the others wait (WC3 mine, SC minerals).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsModule::mechClassDamage(Simulation& sim, Entity attacker, size_t weapon, Entity target, Fixed& dmg, uint16_t& dmgType) const {
    World& w = sim.world();
    if (!w.alive(target) || !w.has<Unit>(target)) return;
    const UnitType& at = rules_.types[w.get<Unit>(attacker).type];
    const UnitType& tt = rules_.types[w.get<Unit>(target).type];
    if (weapon >= at.weapons.size()) return;
    const Fixed base = at.weapons[weapon].damage;
    const Fixed scale = base.raw > 0 ? dmg / base : kOne;   // techs, buffs, squads, height
    const Fixed armorBonus = stat(sim, target, Stat::Armor) - tt.armor;   // armor upgrades raise every base class
    static const std::vector<ClassAttack> none;
    const std::vector<ClassAttack>& atk = weapon < at.mech.attacks.size() ? at.mech.attacks[weapon] : none;
    Fixed total{};
    auto one = [&](uint16_t cls, Fixed amount) {
        const bool isBase = int(cls) < rules_.mech.baseClasses;
        if (!isBase && !(tt.mech.armorClassMask & (uint64_t(1) << cls))) return;   // bonus vs a class it doesn't have
        Fixed armor = cls < tt.mech.armors.size() ? tt.mech.armors[cls] : Fixed{};
        if (isBase) armor += armorBonus;
        total += fxMax(Fixed{}, amount * scale - armor);
    };
    if (atk.empty()) one(0, base);
    else for (auto& a : atk) one(a.cls, a.amount);
    dmg = fxMax(rules_.mech.minDamage, total);
    dmgType = kDamagePure;
}

Fixed RtsModule::mechArmorDamage(Simulation& sim, Entity target, Fixed raw, uint16_t dmgType) const {
    if (dmgType == kDamagePure) return raw;
    const Fixed armor = stat(sim, target, Stat::Armor);
    const MechRules& m = rules_.mech;
    if (m.armorModel == ArmorModel::Percent) {
        if (armor.raw >= 0) {
            Fixed af = armor * m.percentFactor;
            return raw * (kOne - af / (kOne + af));
        }
        // negative armor: damage x (2 - (1 - f)^-a), by whole steps and a linear remainder
        Fixed n = -armor, k = kOne - m.percentFactor, p = kOne;
        int64_t whole = n.floorInt();
        for (int64_t i = 0; i < whole && i < 100; ++i) p = p * k;
        Fixed frac = n - Fixed::fromInt(whole);
        p = p * (kOne - (kOne - k) * frac);
        return raw * (Fixed::fromInt(2) - p);
    }
    // the default fraction is exactly raw / 5, bit-identical with the original formula
    const Fixed floor = m.minDamageFraction == Fixed::fromRatio(1, 5) ? raw / 5 : raw * m.minDamageFraction;
    return fxMax(raw - armor, fxMax(floor, m.minDamage));
}

Fixed RtsModule::mechShieldAbsorb(Simulation& sim, Entity target, Fixed dmg) {
    World& w = sim.world();
    auto* sh = w.tryGet<Shield>(target);
    if (!sh) return dmg;
    sh->lastHit = sim.tick();
    if (sh->hp.raw <= 0) return dmg;
    const Fixed sa = rules_.types[w.get<Unit>(target).type].mech.shieldArmor;
    Fixed eff = fxMax(dmg - sa, Fixed::fromRatio(1, 2));
    if (sh->hp >= eff) { sh->hp -= eff; return Fixed{}; }
    Fixed left = eff - sh->hp;
    sh->hp = Fixed{};
    return left;
}


bool RtsModule::mechNodeHasRoom(Simulation& sim, Entity node, Entity worker) const {
    World& w = sim.world();
    if (!w.alive(node) || !w.has<Unit>(node)) return true;
    const int cap = rules_.types[w.get<Unit>(node).type].mech.maxWorkers;
    if (cap <= 0) return true;
    int n = 0;
    w.each<Orders>([&](Entity e, Orders& ord) {
        if (e == worker || ord.queue.empty()) return;
        const Order& o = ord.queue.front();
        if (o.type == OrderType::Gather && o.target == node && o.state == 2) ++n;
    });
    return n < cap;
}

void RtsModule::mechTickCombat(Simulation& sim) {
    if (!rules_.mech.shields) return;
    World& w = sim.world();
    const Fixed dt = sim.dt();
    std::vector<Entity> add;
    w.each<Unit>([&](Entity e, Unit& u) {
        const MechType& mt = rules_.types[u.type].mech;
        if (mt.shieldMax.raw <= 0 || u.hp.raw <= 0) return;
        auto* sh = w.tryGet<Shield>(e);
        if (!sh) { add.push_back(e); return; }
        if (sim.tick() >= sh->lastHit + uint32_t((mt.shieldDelay * int64_t(sim.tickRate())).floorInt()) && sh->hp < mt.shieldMax)
            sh->hp = fxMin(mt.shieldMax, sh->hp + mt.shieldRegen * dt);
    });
    for (Entity e : add) w.add<Shield>(e, Shield{rules_.types[w.get<Unit>(e).type].mech.shieldMax, 0});
}

} // namespace rts::game
