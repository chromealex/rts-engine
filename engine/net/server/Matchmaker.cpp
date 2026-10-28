// Automatic matchmaking. Queues come from the server config (team count, team size, bot fill,
// rating window). Players queue with the maps their copy of the game has; a match needs a map
// every chosen player has with the same data hash and the right team layout.
#include "ServerInternal.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace rts::net {

const QueueConfig* Server::Impl::queue(const std::string& id) const {
    for (auto& q : cfg.queues) if (q.id == id) return &q;
    return nullptr;
}

static bool mapFits(const MapInfo& m, const QueueConfig& q) {
    if (m.players() != q.teams * q.teamSize) return false;
    std::map<uint8_t, int> perTeam;
    for (auto t : m.slotTeams) ++perTeam[t];
    if (perTeam.size() != q.teams) return false;
    for (auto& [t, n] : perTeam) if (n != q.teamSize) return false;
    return true;
}

static bool sameMap(const MapInfo& a, const MapInfo& b) { return a.file == b.file && a.dataHash == b.dataHash; }

void Server::Impl::onQueueJoin(Client& c, ByteReader& rd) {
    const std::string id = rd.str();
    std::vector<MapInfo> maps;
    uint32_t n = rd.u32();
    for (uint32_t i = 0; i < n && i < 256 && rd.ok(); ++i) { MapInfo m; if (!read(rd, m)) break; maps.push_back(m); }
    const QueueConfig* q = queue(id);
    if (!rd.ok() || !q) { sendError(c, ErrorCode::NoSuchQueue, NetText("net.error.no_such_queue")); return; }
    std::vector<MapInfo> usable;
    for (auto& m : maps) if (mapFits(m, *q)) usable.push_back(m);
    if (usable.empty()) { sendError(c, ErrorCode::BadRequest, NetText("net.error.no_maps_for_queue").str("queue", q->title)); return; }
    if (c.roomId) leaveRoom(c);
    c.queueId = id;
    c.queuedAt = now;
    c.queueMaps = std::move(usable);
    log("client #%u '%s' queued for %s (rating %d)", c.id, c.name.c_str(), id.c_str(), rating(id, c.key).value);
    sendQueueStatus(c, 1);
}

void Server::Impl::leaveQueue(Client& c) {
    c.queueId.clear();
    c.queueMaps.clear();
}

void Server::Impl::sendQueueStatus(Client& c, uint8_t state) {
    QueueStatusMsg s;
    s.state = state;
    s.queueId = c.queueId;
    s.waitedSec = c.queueId.empty() ? 0 : uint32_t((now - c.queuedAt) / 1000);
    for (auto& [id, o] : clients) if (!o->queueId.empty() && o->queueId == c.queueId && o->gameId == c.gameId) ++s.inQueue;
    send(c, encode(s));
}

void Server::Impl::matchmake() {
    const bool status = now - lastQueueStatus >= 2000;
    if (status) lastQueueStatus = now;
    for (auto& q : cfg.queues) {
        std::vector<Client*> waiting;
        for (auto& [id, c] : clients) if (c->queueId == q.id && c->conn) waiting.push_back(c.get());
        std::stable_sort(waiting.begin(), waiting.end(), [](Client* a, Client* b) { return a->queuedAt < b->queuedAt; });
        const size_t need = size_t(q.teams) * q.teamSize;
        std::set<uint32_t> used;
        for (Client* anchor : waiting) {
            if (used.count(anchor->id)) continue;
            const uint64_t waitedSec = (now - anchor->queuedAt) / 1000;
            const int range = int(q.ratingRange + q.ratingWidenPerSec * waitedSec);
            const int base = rating(q.id, anchor->key).value;
            std::vector<Client*> group = {anchor};
            std::vector<MapInfo> common = anchor->queueMaps;
            for (Client* o : waiting) {
                if (group.size() == need) break;
                if (o == anchor || used.count(o->id) || o->gameId != anchor->gameId) continue;
                if (std::abs(rating(q.id, o->key).value - base) > range) continue;
                std::vector<MapInfo> both;
                for (auto& m : common) for (auto& om : o->queueMaps) if (sameMap(m, om)) { both.push_back(m); break; }
                if (both.empty()) continue;
                group.push_back(o);
                common = std::move(both);
            }
            if (group.size() < need && !(q.botFillSec && waitedSec >= q.botFillSec)) continue;
            for (Client* g : group) used.insert(g->id);

            const MapInfo map = common[size_t(rng() % common.size())];
            Room& r = createRoom(q.title, anchor->gameId, map, q.settings);
            r.matchmade = true;
            r.queueId = q.id;
            r.hostId = anchor->id;
            // Balance: strongest first, snake order over teams (1,2,2,1,1,2...).
            std::stable_sort(group.begin(), group.end(), [&](Client* a, Client* b) { return rating(q.id, a->key).value > rating(q.id, b->key).value; });
            std::vector<uint8_t> teamValues;
            for (auto t : map.slotTeams) if (std::find(teamValues.begin(), teamValues.end(), t) == teamValues.end()) teamValues.push_back(t);
            std::sort(teamValues.begin(), teamValues.end());
            auto takeSlot = [&](uint8_t team) -> int {
                for (size_t i = 0; i < map.slotTeams.size(); ++i) if (map.slotTeams[i] == team && r.slots[i].kind == SlotKind::Open) return int(i);
                return -1;
            };
            for (size_t i = 0; i < group.size(); ++i) {
                const size_t round = i / q.teams, pos = i % q.teams;
                const size_t teamIdx = round % 2 == 0 ? pos : q.teams - 1 - pos;
                int slot = takeSlot(teamValues[teamIdx]);
                for (size_t k = 0; slot < 0 && k < teamValues.size(); ++k) slot = takeSlot(teamValues[k]);
                SlotEntry& s = r.slots[size_t(slot)];
                s.kind = SlotKind::Human; s.clientId = group[i]->id; s.name = group[i]->name; s.ready = true;
            }
            for (size_t i = 0; i < r.slots.size(); ++i)
                if (r.slots[i].kind == SlotKind::Open) { r.slots[i].kind = SlotKind::Bot; r.slots[i].name = "@net.bot"; r.slots[i].ready = true; }
            for (Client* g : group) {
                sendQueueStatus(*g, 2);
                leaveQueue(*g);
                g->roomId = r.id;
            }
            log("matchmade %s: %zu players + %zu bots on %s", q.id.c_str(), group.size(), need - group.size(), map.file.c_str());
            startMatch(r);
        }
    }
    if (status) for (auto& [id, c] : clients) if (!c->queueId.empty() && c->conn) sendQueueStatus(*c, 1);
}

std::string Server::Impl::ratingKey(const std::string& queueId, uint64_t key) const {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)key);
    return queueId + ":" + hex;
}

Rating& Server::Impl::rating(const std::string& queueId, uint64_t key) { return ratings[ratingKey(queueId, key)]; }

void Server::Impl::loadRatings() {
    if (cfg.ratingsFile.empty()) return;
    std::ifstream f(cfg.ratingsFile);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream s(line);
        std::string k; Rating r;
        if (s >> k >> r.value >> r.games) ratings[k] = r;
    }
}

void Server::Impl::saveRatings() {
    if (cfg.ratingsFile.empty()) return;
    const std::string tmp = cfg.ratingsFile + ".tmp";
    {
        std::ofstream f(tmp);
        for (auto& [k, r] : ratings) f << k << ' ' << r.value << ' ' << r.games << '\n';
    }
    std::rename(tmp.c_str(), cfg.ratingsFile.c_str());
}

} // namespace rts::net
