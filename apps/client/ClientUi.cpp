// Client UI: the "game" data model behind engine/assets/ui/{menu,hud,lobby}.rml.
// Everything here reads the simulation and turns clicks into Commands; layout and looks are in
// the documents, so games restyle or rearrange the HUD with their own ui/ files.
#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "GameClient.h"
#include "modules/rts/RtsLocalize.h"

namespace rts::client {

using i18n::Args;

namespace {

std::string clockText(int64_t seconds) {
    char b[32];
    std::snprintf(b, sizeof b, "%lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
    return b;
}

std::string fmt(const char* f, auto... args) {
    char b[256];
    std::snprintf(b, sizeof b, f, args...);
    return b;
}

void pushSection(UiState& s, const UiSection& sec) {
    UiAction h;
    h.header = true; h.label = sec.title; h.kind = sec.kind; h.id = -1;
    s.card.push_back(h);
    for (UiAction a : sec.actions) { a.kind = sec.kind; s.card.push_back(std::move(a)); }
}

std::string cssColor(float4 c) {
    return fmt("color: rgb(%d, %d, %d);", int(c.x * 255), int(c.y * 255), int(c.z * 255));
}

class MinimapListener final : public Rml::EventListener {
public:
    explicit MinimapListener(GameClient& c) : client_(c) {}
    void ProcessEvent(Rml::Event& ev) override {
        Rml::Element* el = ev.GetCurrentElement();
        const Rml::Vector2f o = el->GetAbsoluteOffset(Rml::BoxArea::Content);
        const Rml::Vector2f sz = el->GetBox().GetSize(Rml::BoxArea::Content);
        if (sz.x <= 0 || sz.y <= 0) return;
        const float x = ev.GetParameter("mouse_x", 0.0f), y = ev.GetParameter("mouse_y", 0.0f);
        client_.minimapPointer(std::clamp((x - o.x) / sz.x, 0.0f, 1.0f), std::clamp((y - o.y) / sz.y, 0.0f, 1.0f));
    }

private:
    GameClient& client_;
};

} // namespace

void GameClient::initUi() {
    if (!ui_.init(*dev_, "fonts/ui.ttf")) { error_ = "ui: init failed"; return; }
    ui_.setKeyboardLanguage(loc_.language());
    Rml::DataModelConstructor c = ui_.context()->CreateDataModel("game");
    if (!c) return;
    if (auto h = c.RegisterStruct<UiItem>()) {
        h.RegisterMember("text", &UiItem::text); h.RegisterMember("sub", &UiItem::sub);
        h.RegisterMember("style", &UiItem::style); h.RegisterMember("warn", &UiItem::warn);
    }
    if (auto h = c.RegisterStruct<UiAction>()) {
        h.RegisterMember("label", &UiAction::label); h.RegisterMember("cost", &UiAction::cost);
        h.RegisterMember("hotkey", &UiAction::hotkey); h.RegisterMember("tip", &UiAction::tip);
        h.RegisterMember("enabled", &UiAction::enabled); h.RegisterMember("active", &UiAction::active);
        h.RegisterMember("progress", &UiAction::progress); h.RegisterMember("id", &UiAction::id);
        h.RegisterMember("kind", &UiAction::kind); h.RegisterMember("header", &UiAction::header);
        h.RegisterMember("alt", &UiAction::alt); h.RegisterMember("altLabel", &UiAction::altLabel);
    }
    c.RegisterArray<std::vector<UiAction>>();
    c.RegisterArray<std::vector<UiItem>>();
    c.RegisterArray<std::vector<std::string>>();

    UiState& b = bound_;
    c.Bind("screen", &b.screen); c.Bind("gameName", &b.gameName); c.Bind("error", &b.error);
    c.Bind("platformInfo", &b.platformInfo); c.Bind("maps", &b.maps);
    c.Bind("top", &b.top); c.Bind("topActions", &b.topActions); c.Bind("clock", &b.clock); c.Bind("notices", &b.notices);
    c.Bind("hasSelection", &b.hasSelection); c.Bind("ownSelection", &b.ownSelection);
    c.Bind("selTitle", &b.selTitle); c.Bind("emptyHint", &b.emptyHint); c.Bind("selInfo", &b.selInfo);
    c.Bind("card", &b.card); c.Bind("overTitle", &b.overTitle); c.Bind("overTime", &b.overTime);
    c.Bind("modeHint", &b.modeHint); c.Bind("hintStyle", &b.hintStyle); c.Bind("boxStyle", &b.boxStyle);
    c.Bind("minimapStyle", &b.minimapStyle); c.Bind("lines", &b.lines); c.Bind("touch", &b.touch); c.Bind("showCard", &b.showCard);
    c.Bind("languages", &b.languages);
    c.Bind("lobby", &b.lobby); c.Bind("netStatus", &b.netStatus); c.Bind("netError", &b.netError);
    c.Bind("roomTitle", &b.roomTitle); c.Bind("roomInfo", &b.roomInfo); c.Bind("spectators", &b.spectators); c.Bind("waitHint", &b.waitHint);
    c.Bind("queues", &b.queues); c.Bind("rooms", &b.rooms); c.Bind("createMaps", &b.createMaps); c.Bind("slots", &b.slots);
    c.Bind("chat", &b.chat); c.Bind("isHost", &b.isHost); c.Bind("myReady", &b.myReady); c.Bind("canStart", &b.canStart);
    c.Bind("netMatch", &b.netMatch); c.Bind("searching", &b.searching);
    // two-way bindings (menu controls write straight into the client)
    c.Bind("difficulty", &difficulty_);
    c.Bind("spectator", &spectator_);
    c.Bind("shadows", &shadowQuality_);
    c.Bind("volume", &volume_);
    c.Bind("music", &musicVolume_);
    c.Bind("netAddress", &netAddress_);
    c.Bind("netName", &netName_);
    c.Bind("chatInput", &chatInput_);
    c.Bind("roomName", &roomName_);

    using VArgs = const Rml::VariantList&;
    c.BindEventCallback("start", [this](Rml::DataModelHandle, Rml::Event&, VArgs a) { if (!a.empty()) startMatch(size_t(a[0].Get<int>())); });
    c.BindEventCallback("act", [this](Rml::DataModelHandle, Rml::Event&, VArgs a) {
        if (a.empty()) return;
        const int id = a[0].Get<int>(-1);
        if (id >= 0 && size_t(id) < actions_.size() && actions_[size_t(id)]) actions_[size_t(id)]();
    });
    c.BindEventCallback("pause", [this](Rml::DataModelHandle, Rml::Event&, VArgs) { if (screen_ == Screen::Playing) setScreen(Screen::Paused); });
    c.BindEventCallback("resume", [this](Rml::DataModelHandle, Rml::Event&, VArgs) { if (screen_ == Screen::Paused) setScreen(Screen::Playing); });
    c.BindEventCallback("restart", [this](Rml::DataModelHandle, Rml::Event&, VArgs) { startMatch(mapIndex_); });
    c.BindEventCallback("tomenu", [this](Rml::DataModelHandle, Rml::Event&, VArgs) {
        if (netMatch_) { leaveNetMatch(); return; }
        toMenu();
    });
    // net('connect'), net('create', i) ... -> Lobby.cpp
    c.BindEventCallback("net", [this](Rml::DataModelHandle, Rml::Event&, VArgs a) {
        if (!a.empty()) netAction(a[0].Get<Rml::String>(), a.size() > 1 ? a[1].Get<int>(-1) : -1);
    });
    model_ = c.GetModelHandle();

    shadowQuality_ = boundShadowQuality_ = int(r3d_.shadowQuality());
    ui_.setTranslator([this](const std::string& key) { return tr(key); });
    minimapListener_ = std::make_unique<MinimapListener>(*this);
    loadDocuments();
}

// (Re)loads every document: "@key" texts in them are translated at load, so a language change
// reloads them. The data model survives; documents rebind to it.
void GameClient::loadDocuments() {
    if (minimapEl_) {
        minimapEl_->RemoveEventListener(Rml::EventId::Mousedown, minimapListener_.get());
        minimapEl_->RemoveEventListener(Rml::EventId::Drag, minimapListener_.get());
        minimapEl_ = nullptr;
    }
    for (Rml::ElementDocument* d : {menuDoc_, hudDoc_, lobbyDoc_}) if (d) d->Close();
    menuDoc_ = ui_.loadDocument("menu.rml");
    lobbyDoc_ = ui_.loadDocument("lobby.rml");
    hudDoc_ = ui_.loadDocument("hud.rml");
    if (!menuDoc_ || !hudDoc_ || !lobbyDoc_) error_ = "ui: documents not found (engine/ui/menu.rml, hud.rml, lobby.rml)";
    if (hudDoc_) minimapEl_ = hudDoc_->GetElementById("minimap-img");
    if (minimapEl_) {
        minimapEl_->AddEventListener(Rml::EventId::Mousedown, minimapListener_.get());
        minimapEl_->AddEventListener(Rml::EventId::Drag, minimapListener_.get());
    }
}

void GameClient::shutdownUi() {
    if (minimapEl_ && minimapListener_) {
        minimapEl_->RemoveEventListener(Rml::EventId::Mousedown, minimapListener_.get());
        minimapEl_->RemoveEventListener(Rml::EventId::Drag, minimapListener_.get());
    }
    minimapEl_ = nullptr;
    menuDoc_ = hudDoc_ = lobbyDoc_ = nullptr;
    ui_.shutdown();
    minimapListener_.reset();
}

void GameClient::setScreen(Screen s) {
    screen_ = s;
    if (s != Screen::Playing) { boxing_ = panning_ = sendDrag_ = false; mode_ = Mode::Normal; }
}

void GameClient::minimapPointer(float u, float v) {
    if (!sim_) return;
    const auto& g = mod().state(*sim_).grid;
    cam_.target = {u * float(g.w), 0, v * float(g.h)};
}

// ------------------------------------------------------------------ per-frame model

void GameClient::updateUi() {
    UiState s;
    s.screen = screen_ == Screen::Menu ? "menu" : screen_ == Screen::Lobby ? "lobby" : screen_ == Screen::Playing ? "playing"
             : screen_ == Screen::Paused ? "paused" : "over";
    s.gameName = gameName_;
    s.error = error_;
    s.platformInfo = std::string(platform::name()) + " · " + dev_->info().name;
    for (auto& m : maps_) s.maps.push_back(m.title);
    s.touch = platform::isTouchDevice();
    s.showCard = !spectator_;
    nextActions_.clear();
    if (langs_.available.size() > 1)
        for (size_t i = 0; i < langs_.available.size() && i < langNames_.size(); ++i) {
            UiAction a;
            a.label = langNames_[i];
            a.active = langs_.available[i] == loc_.language();
            const std::string tag = langs_.available[i];
            a.id = addAction([this, tag] { applyLanguage(tag, true); reloadDocs_ = true; }, !a.active);
            s.languages.push_back(a);
        }
    if (screen_ == Screen::Lobby || net_) buildLobby(s);

    if (sim_) {
        cleanSelection();
        buildTopBar(s);
        if (!spectator_) buildCommandCard(s);
        buildOverlays(s);
        for (auto& n : notices_) if (n.until >= time_) s.notices.push_back(n.text);
        if (screen_ == Screen::Over) {
            const int res = sim_->result();
            auto* me = sim_->player(human_);
            const bool won = !spectator_ && res > 0 && me && me->team == res;
            s.overTitle = tr(spectator_ ? "pause.match_over" : won ? "pause.victory" : "pause.defeat");
            s.overTime = tr("pause.time", Args().str("time", clockText(sim_->tick() / sim_->tickRate())));
        }
        // minimap image
        auto& gs = mod().state(*sim_);
        const float W = platform::windowWidth(), H = platform::windowHeight();
        float3 q[4];
        const bool haveQuad = groundAt(0, 0, q[0]) && groundAt(W, 0, q[1]) && groundAt(W, H, q[2]) && groundAt(0, H, q[3]);
        minimap_.update(*dev_, gs, rules_, scene_, spectator_ ? nullptr : &gs.vision[human_], q, haveQuad);
        const float side = s.touch ? 150.0f : 190.0f;
        const float k = side / float(std::max(gs.grid.w, gs.grid.h));
        s.minimapStyle = fmt("width: %.0fdp; height: %.0fdp;", gs.grid.w * k, gs.grid.h * k);
    }

    actions_.swap(nextActions_);
    // push only what changed: unchanged variables keep their elements (and hover/press state)
#define RTS_SYNC(name)                                       \
    if (!(s.name == bound_.name)) {                          \
        bound_.name = std::move(s.name);                     \
        model_.DirtyVariable(#name);                         \
    }
    if (model_) {
        RTS_SYNC(screen) RTS_SYNC(gameName) RTS_SYNC(error) RTS_SYNC(platformInfo) RTS_SYNC(maps)
        RTS_SYNC(top) RTS_SYNC(topActions) RTS_SYNC(clock) RTS_SYNC(notices) RTS_SYNC(hasSelection) RTS_SYNC(ownSelection)
        RTS_SYNC(selTitle) RTS_SYNC(emptyHint) RTS_SYNC(selInfo) RTS_SYNC(card) RTS_SYNC(overTitle) RTS_SYNC(overTime)
        RTS_SYNC(modeHint) RTS_SYNC(hintStyle) RTS_SYNC(boxStyle) RTS_SYNC(minimapStyle) RTS_SYNC(lines) RTS_SYNC(touch) RTS_SYNC(showCard)
        RTS_SYNC(languages) RTS_SYNC(lobby) RTS_SYNC(netStatus) RTS_SYNC(netError) RTS_SYNC(roomTitle) RTS_SYNC(roomInfo)
        RTS_SYNC(spectators) RTS_SYNC(waitHint) RTS_SYNC(queues) RTS_SYNC(rooms) RTS_SYNC(createMaps) RTS_SYNC(slots) RTS_SYNC(chat)
        RTS_SYNC(isHost) RTS_SYNC(myReady) RTS_SYNC(canStart) RTS_SYNC(netMatch) RTS_SYNC(searching)
        if (shadowQuality_ != boundShadowQuality_) { // changed from the pause menu
            r3d_.setShadowQuality(ShadowQuality(std::clamp(shadowQuality_, 0, 3)));
            boundShadowQuality_ = shadowQuality_;
        }
    }
#undef RTS_SYNC
    if (reloadDocs_) { reloadDocs_ = false; loadDocuments(); }
    ui_.update();
}

void GameClient::buildTopBar(UiState& s) {
    auto& gs = mod().state(*sim_);
    const auto& eco = gs.players[spectator_ ? 1 : human_];
    for (size_t r = 0; r < rules_.resources.size(); ++r) {
        const auto& rd = rules_.resources[r];
        UiItem it;
        if (rd.kind == ResourceKind::Capacity) {
            it.text = fmt("%s: %lld/%lld", rd.name.c_str(), (long long)eco.capUsed[r].toInt(), (long long)eco.capProvided[r].toInt());
            it.warn = eco.capUsed[r] > eco.capProvided[r];
        } else {
            it.text = fmt("%s: %lld", rd.name.c_str(), (long long)eco.stock[r].toInt());
            if ((rd.kind == ResourceKind::Flow || rd.capped) && eco.cap[r].raw > 0) it.sub = fmt("/%lld", (long long)eco.cap[r].toInt());
            if (rd.kind == ResourceKind::Flow) it.sub += " " + tr("hud.per_second", Args().str("value", fmt("%+.1f", eco.income[r].toDouble())));
        }
        s.top.push_back(it);
    }
    if (eco.supplyCap.raw > 0 || eco.supplyUsed.raw > 0)
        s.top.push_back({tr("hud.supply", Args().num("used", eco.supplyUsed.toInt()).num("cap", eco.supplyCap.toInt())), "", "", eco.supplyUsed >= eco.supplyCap});
    if (rules_.resources.empty()) {
        for (auto& p : sim_->players()) {
            int g = 0;
            sim_->world().each<Garrison, Owner>([&](Entity, Garrison& gg, Owner& o) { if (o.player == p.slot) g += gg.count; });
            sim_->world().each<Unit, Owner>([&](Entity en, Unit&, Owner& o) { if (o.player == p.slot && sim_->world().has<Mobile>(en)) ++g; });
            const std::string name = netMatch_ || mapIndex_ >= maps_.size() ? p.name : localizedPlayerName(loc_, maps_[mapIndex_].file, size_t(p.slot - 1), p.name);
            s.top.push_back({fmt("%s %d", name.c_str(), g) + (p.alive ? "" : tr("hud.eliminated")), "", cssColor(RtsView::playerColor(p.slot)), false});
        }
    }
    const auto& vic = gs.victory;
    if (vic.mode == VictoryMode::Tickets)
        for (size_t t = 1; t < gs.tickets.size(); ++t) s.top.push_back({tr("hud.team", Args().num("team", int64_t(t)).num("tickets", gs.tickets[t].toInt())), "", "", false});
    if (vic.mode == VictoryMode::Score)
        for (size_t t = 1; t < gs.score.size(); ++t)
            s.top.push_back({fmt("%lld", (long long)gs.score[t].toInt()), "", cssColor(RtsView::playerColor(uint8_t(t))), false});
    if (vic.mode == VictoryMode::Survive || vic.mode == VictoryMode::Score) {
        int64_t left = std::max<int64_t>(0, (vic.surviveSeconds * int64_t(sim_->tickRate())).toInt() - int64_t(sim_->tick())) / sim_->tickRate();
        s.top.push_back({tr("hud.time_left", Args().str("time", clockText(left))), "", "", left < 30});
    }
    if (uint32_t wt = mod().ticksUntilWave(*sim_)) s.top.push_back({tr("hud.wave_in", Args().num("count", wt / sim_->tickRate() + 1)), "", "", false});
    if (gs.regionCount > 0 && !spectator_) {
        int owned = 0;
        for (auto o : gs.regionOwner) if (o == human_) ++owned;
        s.top.push_back({tr("hud.regions", Args().num("count", owned)), "", "", false});
        UiAction a;
        a.label = tr("hud.claim"); a.active = mode_ == Mode::Claim;
        a.id = addAction([this] { mode_ = Mode::Claim; });
        s.topActions.push_back(a);
    }
    if (!rules_.autoBuildPool.empty() && !spectator_) {
        UiAction a;
        a.label = tr("hud.auto_build");
        for (auto& c : mod().autoBuildCost(*sim_, human_, rules_.autoBuildPool[0]))
            a.cost += (a.cost.empty() ? "" : " ") + std::to_string(c.value.toInt()) + " " + rules_.resources[c.res].name;
        a.id = addAction([this] { Command c; c.type = kCmdAutoBuild; c.param = -1; issue(c); });
        s.topActions.push_back(a);
    }
    s.clock = fmt("%u:%02u", sim_->tick() / sim_->tickRate() / 60, sim_->tick() / sim_->tickRate() % 60);
}

void GameClient::buildCommandCard(UiState& s) {
    World& w = sim_->world();
    if (selection_.empty()) {
        s.emptyHint = tr(platform::isTouchDevice() ? "card.hint_touch" : "card.hint_mouse");
        UiSection sec{"", "list", {}};
        UiAction a;
        a.label = tr("card.all_army");
        a.id = addAction([this] {
            for (auto& o : scene_.objects)
                if (o.owner == human_ && !o.isProjectile && !rules_.types[o.type].structure && !rules_.types[o.type].worker && !rules_.types[o.type].weapons.empty())
                    selection_.push_back(o.entity);
        });
        sec.actions.push_back(a);
        pushSection(s, sec);
        return;
    }
    s.hasSelection = true;
    std::map<uint16_t, int> counts;
    for (Entity e : selection_) counts[w.get<Unit>(e).type]++;
    for (auto& [t, n] : counts) {
        if (!s.selTitle.empty()) s.selTitle += ", ";
        s.selTitle += n > 1 ? fmt("%s ×%d", rules_.types[t].name.c_str(), n) : rules_.types[t].name;
    }
    const Entity e0 = selection_[0];
    const UnitType& t0 = rules_.types[w.get<Unit>(e0).type];
    if (selection_.size() == 1) {
        const Unit& u = w.get<Unit>(e0);
        if (!t0.invulnerable) s.selInfo.push_back(tr("card.health", Args().num("hp", u.hp.toInt()).num("max", t0.maxHp.toInt())));
        if (auto* rs = w.tryGet<ResourceStock>(e0)) s.selInfo.push_back(tr("card.stock", Args().num("amount", rs->amount.toInt())));
        if (auto* g = w.tryGet<Garrison>(e0)) s.selInfo.push_back(tr("card.garrison", Args().num("count", g->count).num("max", t0.garrisonMax)));
    }
    if (w.get<Owner>(e0).player != human_) return;
    s.ownSelection = true;

    auto costText = [&](uint16_t type) {
        std::string c;
        for (auto& x : rules_.types[type].cost) c += (c.empty() ? "" : "  ") + std::to_string(x.value.toInt()) + " " + rules_.resources[x.res].name;
        if (rules_.types[type].supplyUsed.raw > 0) c += (c.empty() ? "" : "  ") + tr("card.supply_cost", Args().num("count", rules_.types[type].supplyUsed.toInt()));
        return c;
    };
    auto reqText = [&](const std::vector<Requirement>& req) {
        std::string r;
        for (auto& q : req) r += (r.empty() ? "" : ", ") + (q.tech ? rules_.techs[q.index].name : rules_.types[q.index].name);
        return r.empty() ? r : tr("card.requires", Args().str("list", r));
    };
    auto simple = [&](uint16_t type, std::vector<Entity> units, int64_t param = 0) {
        return [this, type, units, param] { Command c; c.type = type; c.units = units; c.param = param; issue(c); };
    };

    if (selectionHasGarrison()) {
        UiSection sec{tr("card.send"), "row", {}};
        for (int p : {25, 50, 75, 100}) {
            UiAction a;
            a.label = fmt("%d%%", p); a.active = sendPercent_ == p; a.hotkey = std::to_string(p / 25);
            a.id = addAction([this, p] { sendPercent_ = p; });
            sec.actions.push_back(a);
        }
        pushSection(s, sec);
        s.selInfo.push_back(tr(platform::isTouchDevice() ? "ui.send_hint_touch" : "card.send_hint"));
    }
    bool anyMobile = false, anyWeapon = false;
    std::vector<uint16_t> buildable;
    for (Entity e : selection_) {
        const UnitType& t = rules_.types[w.get<Unit>(e).type];
        if (t.speed.raw > 0) anyMobile = true;
        if (!t.weapons.empty()) anyWeapon = true;
        for (uint16_t b : t.builds) if (std::find(buildable.begin(), buildable.end(), b) == buildable.end()) buildable.push_back(b);
    }
    if (anyMobile) {
        UiSection sec{"", "row", {}};
        auto add = [&](const std::string& label, const char* key, bool active, std::function<void()> fn) {
            UiAction a; a.label = label; a.hotkey = key; a.active = active; a.id = addAction(std::move(fn));
            sec.actions.push_back(a);
        };
        if (anyWeapon) add(tr("card.attack"), "", mode_ == Mode::AttackMove, [this] { mode_ = Mode::AttackMove; });
        add(tr("card.stop"), "", false, simple(kCmdStop, selection_));
        add(tr("card.hold"), "", false, simple(kCmdHold, selection_));
        add(tr("card.patrol"), "", mode_ == Mode::Patrol, [this] { mode_ = Mode::Patrol; });
        pushSection(s, sec);
    }
    if (!buildable.empty()) {
        UiSection sec{tr("card.build"), "grid", {}};
        for (uint16_t b : buildable) {
            UiAction a;
            a.label = rules_.types[b].name; a.cost = costText(b);
            a.enabled = mod().canTrain(*sim_, human_, b);
            if (!a.enabled) a.tip = reqText(rules_.types[b].requires_);
            a.active = mode_ == Mode::Place && placeType_ == b && !placeProducer_.valid();
            a.id = addAction([this, b] { mode_ = Mode::Place; placeType_ = b; placeProducer_ = kNullEntity; }, a.enabled);
            sec.actions.push_back(a);
        }
        pushSection(s, sec);
    }
    if (!t0.abilities.empty()) {
        UiSection sec{tr("ui.abilities"), "grid", {}};
        for (size_t i = 0; i < t0.abilities.size(); ++i) {
            const AbilityDef& ab = t0.abilities[i];
            if (ab.aura) { s.selInfo.push_back(tr("card.aura", Args().str("name", ab.name))); continue; }
            UiAction a;
            a.label = ab.name;
            if (ab.energyCost.raw > 0) a.cost = tr("card.energy_cost", Args().num("count", ab.energyCost.toInt()));
            a.enabled = mod().canCast(*sim_, e0, int(i));
            a.active = mode_ == Mode::Ability && abilityIndex_ == int(i);
            const bool instant = ab.target == TargetKind::None || ab.target == TargetKind::Self;
            a.id = addAction(instant ? simple(kCmdAbility, selection_, int64_t(i)) : std::function<void()>([this, i] { mode_ = Mode::Ability; abilityIndex_ = int(i); }), a.enabled);
            sec.actions.push_back(a);
        }
        if (!sec.actions.empty()) pushSection(s, sec);
    }
    if (auto* prod = w.tryGet<Production>(e0); prod && !(w.get<Unit>(e0).flags & kUnitUnderConstruction)) {
        UiSection sec{tr("ui.production"), "grid", {}};
        if (prod->ready != kNoType) {
            UiAction a;
            a.label = tr("card.place", Args().str("name", rules_.types[prod->ready].name));
            a.active = mode_ == Mode::Place && placeProducer_ == e0;
            uint16_t ready = prod->ready;
            a.id = addAction([this, ready, e0] { mode_ = Mode::Place; placeType_ = ready; placeProducer_ = e0; });
            sec.actions.push_back(a);
        }
        if (t0.chargesMax > 0) s.selInfo.push_back(tr("card.charges", Args().num("count", prod->charges).num("max", t0.chargesMax)));
        for (uint16_t p : t0.produces) {
            UiAction a;
            a.label = rules_.types[p].name; a.cost = costText(p);
            a.enabled = mod().canTrain(*sim_, human_, p);
            if (!a.enabled) a.tip = reqText(rules_.types[p].requires_);
            a.id = addAction([this, p, e0] {
                World& ww = sim_->world();
                if (!ww.alive(e0) || !ww.has<Unit>(e0)) return;
                // queue on the least busy producer of the same type in the selection
                Entity best = e0; size_t bestQ = SIZE_MAX;
                for (Entity e : selection_)
                    if (ww.alive(e) && ww.has<Unit>(e) && ww.get<Unit>(e).type == ww.get<Unit>(e0).type)
                        if (auto* pp = ww.tryGet<Production>(e); pp && pp->queue.size() < bestQ) { bestQ = pp->queue.size(); best = e; }
                Command c; c.type = kCmdTrain; c.param = p; c.units = {best};
                issue(c);
            }, a.enabled);
            sec.actions.push_back(a);
        }
        const auto& eco = mod().state(*sim_).players[human_];
        for (uint16_t k : t0.researches) {
            const TechDef& tech = rules_.techs[k];
            if (eco.researched[k]) continue;
            UiAction a;
            a.label = tr("card.research", Args().str("name", tech.name));
            for (auto& c : tech.cost) a.cost += (a.cost.empty() ? "" : "  ") + std::to_string(c.value.toInt()) + " " + rules_.resources[c.res].name;
            a.enabled = !eco.researching[k] && mod().requirementsMet(*sim_, human_, tech.requires_);
            if (!a.enabled && !eco.researching[k]) a.tip = reqText(tech.requires_);
            a.id = addAction(simple(kCmdResearch, {e0}, k), a.enabled);
            sec.actions.push_back(a);
        }
        for (uint16_t up : t0.upgrades) {
            UiAction a;
            a.label = tr("card.upgrade", Args().str("name", rules_.types[up].name)); a.cost = costText(up);
            a.id = addAction(simple(kCmdUpgrade, {e0}, up));
            sec.actions.push_back(a);
        }
        if (!t0.produces.empty()) {
            UiAction a;
            a.label = tr("card.rally"); a.active = mode_ == Mode::Rally;
            a.id = addAction([this] { mode_ = Mode::Rally; });
            sec.actions.push_back(a);
        }
        if (!sec.actions.empty()) pushSection(s, sec);
        if (!prod->queue.empty()) {
            UiSection q{tr("ui.queue"), "queue", {}};
            for (size_t i = 0; i < prod->queue.size(); ++i) {
                const ProdItem& it = prod->queue[i];
                UiAction a;
                a.label = it.item >= 0 ? rules_.types[size_t(it.item)].name : rules_.techs[size_t(~it.item)].name;
                if (i == 0 || t0.chargesMax > 0) a.progress = float(std::clamp(it.progress.toDouble(), 0.0, 1.0));
                a.id = addAction(simple(kCmdCancelTrain, {e0}, int64_t(i)));
                q.actions.push_back(a);
            }
            pushSection(s, q);
        }
    }
    UiSection special{"", "row", {}};
    auto addSpecial = [&](const std::string& label, std::function<void()> fn) {
        UiAction a; a.label = label; a.id = addAction(std::move(fn)); special.actions.push_back(a);
    };
    if (t0.structure && t0.sellable && !w.has<Construction>(e0)) addSpecial(tr("card.sell"), simple(kCmdSell, {e0}));
    if (t0.deploysTo != kNoType) addSpecial(tr("card.deploy"), simple(kCmdDeploy, {e0}));
    if (t0.members > 1) {
        s.selInfo.push_back(tr("card.squad", Args().num("alive", mod().membersAlive(*sim_, e0)).num("max", t0.members)));
        addSpecial(tr("card.reinforce"), simple(kCmdReinforce, selection_));
    }
    if (auto* h = w.tryGet<Hero>(e0)) s.selInfo.push_back(tr("card.hero", Args().num("level", h->level).num("xp", h->xp.toInt())));
    if (t0.maxEnergy.raw > 0) s.selInfo.push_back(tr("card.energy", Args().num("energy", w.get<Unit>(e0).energy.toInt()).num("max", t0.maxEnergy.toInt())));
    if (w.has<Construction>(e0)) addSpecial(tr("card.cancel_build"), simple(kCmdCancelTrain, {e0}));
    if (!special.actions.empty()) pushSection(s, special);
}

void GameClient::buildOverlays(UiState& s) {
    const float k = platform::dpiScale(); // logical points -> context (framebuffer) pixels
    if (boxing_ && std::hypot(mouseX_ - downX_, mouseY_ - downY_) > 6) {
        const float x0 = std::min(downX_, mouseX_), y0 = std::min(downY_, mouseY_);
        s.boxStyle = fmt("left: %.0fpx; top: %.0fpx; width: %.0fpx; height: %.0fpx;", x0 * k, y0 * k,
                         std::abs(mouseX_ - downX_) * k, std::abs(mouseY_ - downY_) * k);
    }
    if (sendDrag_) {
        for (Entity e : selection_) {
            auto* t = sim_->world().tryGet<Transform>(e);
            float2 a;
            if (!t || !cam_.worldToScreen({float(t->pos.x.toDouble()), 0.5f, float(t->pos.z.toDouble())}, platform::windowWidth(), platform::windowHeight(), a))
                continue;
            const float dx = (mouseX_ - a.x) * k, dy = (mouseY_ - a.y) * k;
            s.lines.push_back(fmt("left: %.1fpx; top: %.1fpx; width: %.1fpx; transform: rotate(%.2fdeg);", a.x * k, a.y * k,
                                  std::hypot(dx, dy), std::atan2(dy, dx) * 57.29578f));
        }
    }
    if (mode_ != Mode::Normal && screen_ == Screen::Playing) {
        s.modeHint = tr(mode_ == Mode::AttackMove ? "mode.attack" : mode_ == Mode::Patrol ? "mode.patrol"
                      : mode_ == Mode::Rally ? "mode.rally" : mode_ == Mode::Ability ? "mode.ability"
                      : mode_ == Mode::Claim ? "mode.claim" : "mode.place");
        s.hintStyle = fmt("left: %.0fpx; top: %.0fpx;", (mouseX_ + 16) * k, (mouseY_ + 8) * k);
    }
}

} // namespace rts::client
