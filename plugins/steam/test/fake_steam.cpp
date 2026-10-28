// Fake libsteam_api: just enough of the Steam client (inventory, overlay, DLC) to test
// plugins/steam/src/SteamPlugin.cpp without Steam. Built and run by run.sh.
#define STEAM_API_EXPORTS
#include <steam/steam_api.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static std::vector<SteamItemDetails_t> g_items;
static SteamItemInstanceID_t g_nextItem = 1000;
static std::map<SteamInventoryResult_t, std::vector<SteamItemDetails_t>> g_results;
static SteamInventoryResult_t g_nextResult = 1;
static SteamAPICall_t g_nextCall = 1;
static std::set<AppId_t> g_dlc;
static SteamItemDef_t g_pendingDef = 0;
static AppId_t g_storeApp = 0;
struct Ev { int id; SteamAPICall_t call; std::vector<char> data; };
static std::vector<Ev> g_queue;
static std::map<int, std::vector<CCallbackBase*>> g_cbs;
static std::map<SteamAPICall_t, CCallbackBase*> g_calls;

template <class T> static void queueCb(const T& t) { Ev e{T::k_iCallback, 0, std::vector<char>(sizeof t)}; std::memcpy(e.data.data(), &t, sizeof t); g_queue.push_back(e); }
template <class T> static SteamAPICall_t queueCall(const T& t) { Ev e{T::k_iCallback, g_nextCall++, std::vector<char>(sizeof t)}; std::memcpy(e.data.data(), &t, sizeof t); g_queue.push_back(e); return e.call; }

static SteamInventoryResult_t newResult(std::vector<SteamItemDetails_t> v) {
    SteamInventoryResult_t h = g_nextResult++;
    g_results[h] = std::move(v);
    SteamInventoryResultReady_t r{}; r.m_handle = h; r.m_result = k_EResultOK; queueCb(r);
    return h;
}
static bool fake_GetResultItems(SteamInventoryResult_t h, SteamItemDetails_t* out, uint32* n) {
    auto it = g_results.find(h); if (it == g_results.end()) return false;
    if (out) { uint32 c = std::min<uint32>(*n, uint32(it->second.size())); std::memcpy(out, it->second.data(), c * sizeof *out); *n = c; }
    else *n = uint32(it->second.size());
    return true;
}
static void fake_destroy(SteamInventoryResult_t h) { g_results.erase(h); }
static bool fake_GetAllItems(SteamInventoryResult_t* h) { *h = newResult(g_items); return true; }
static bool fake_ConsumeItem(SteamInventoryResult_t* h, SteamItemInstanceID_t id, uint32 q) {
    for (size_t i = 0; i < g_items.size(); ++i) if (g_items[i].m_itemId == id) {
        if (g_items[i].m_unQuantity > q) g_items[i].m_unQuantity = uint16(g_items[i].m_unQuantity - q); else g_items.erase(g_items.begin() + long(i));
        *h = newResult({}); return true;
    }
    return false;
}
static bool fake_DefProperty(SteamItemDef_t d, const char* name, char* buf, uint32* size) {
    std::string v = std::string(name) == "name" ? "Gems " + std::to_string(d) : "Pack of gems";
    if (buf) { std::snprintf(buf, *size, "%s", v.c_str()); } *size = uint32(v.size() + 1); return true;
}
static SteamAPICall_t fake_StartPurchase(const SteamItemDef_t* defs, const uint32*, uint32) {
    g_pendingDef = defs[0];
    SteamInventoryStartPurchaseResult_t r{}; r.m_result = k_EResultOK; r.m_ulOrderID = 555; r.m_ulTransID = 666;
    return queueCall(r);
}
static SteamAPICall_t fake_RequestPrices() { SteamInventoryRequestPricesResult_t r{}; r.m_result = k_EResultOK; std::memcpy(r.m_rgchCurrency, "USD", 4); return queueCall(r); }
static bool fake_GetItemPrice(SteamItemDef_t d, uint64* cur, uint64* base) { if (d != 100 && d != 200) return false; *cur = d == 100 ? 499 : 1999; *base = *cur; return true; }
static bool fake_dlcOwned(AppId_t a) { return g_dlc.count(a) > 0; }
static void fake_overlayStore(AppId_t a) { g_storeApp = a; }

#include "fakes.inc"
static FakeISteamInventory g_inv; static FakeISteamApps g_apps; static FakeISteamFriends g_friends; static FakeISteamUser g_user; static FakeISteamUtils g_utils;

class CCallbackMgr {
public:
    static void setRegistered(CCallbackBase* cb, int id, bool on) { if (on) { cb->m_nCallbackFlags |= CCallbackBase::k_ECallbackFlagsRegistered; cb->m_iCallback = id; } else cb->m_nCallbackFlags &= uint8(~CCallbackBase::k_ECallbackFlagsRegistered); }
};

extern "C" {
S_API ESteamAPIInitResult S_CALLTYPE SteamInternal_SteamAPI_Init(const char*, SteamErrMsg* err) {
    if (std::getenv("FAKE_STEAM_FAIL")) { if (err) std::snprintf(*err, sizeof *err, "fake: no client"); return k_ESteamAPIInitResult_NoSteamClient; }
    return k_ESteamAPIInitResult_OK;
}
S_API ESteamAPIInitResult S_CALLTYPE SteamAPI_InitFlat(SteamErrMsg* e) { return SteamInternal_SteamAPI_Init("", e); }
S_API void S_CALLTYPE SteamAPI_Shutdown() {}
S_API bool S_CALLTYPE SteamAPI_RestartAppIfNecessary(uint32) { return false; }
S_API HSteamUser S_CALLTYPE SteamAPI_GetHSteamUser() { return 1; }
S_API HSteamPipe S_CALLTYPE SteamAPI_GetHSteamPipe() { return 1; }
S_API void* S_CALLTYPE SteamInternal_ContextInit(void* p) {
    void** c = static_cast<void**>(p);
    if (!c[1]) { c[1] = (void*)1; reinterpret_cast<void (*)(void*)>(c[0])(&c[2]); }
    return &c[2];
}
S_API void* S_CALLTYPE SteamInternal_FindOrCreateUserInterface(HSteamUser, const char* v) {
    std::string s(v);
    if (s.rfind("STEAMINVENTORY", 0) == 0) return static_cast<ISteamInventory*>(&g_inv);
    if (s.rfind("STEAMAPPS", 0) == 0) return static_cast<ISteamApps*>(&g_apps);
    if (s.rfind("SteamFriends", 0) == 0) return static_cast<ISteamFriends*>(&g_friends);
    if (s.rfind("SteamUser", 0) == 0) return static_cast<ISteamUser*>(&g_user);
    if (s.rfind("SteamUtils", 0) == 0) return static_cast<ISteamUtils*>(&g_utils);
    std::fprintf(stderr, "fake steam: unknown interface %s\n", v);
    return nullptr;
}
S_API void S_CALLTYPE SteamAPI_RegisterCallback(CCallbackBase* cb, int id) { CCallbackMgr::setRegistered(cb, id, true); g_cbs[id].push_back(cb); }
S_API void S_CALLTYPE SteamAPI_UnregisterCallback(CCallbackBase* cb) {
    CCallbackMgr::setRegistered(cb, 0, false);
    for (auto& [id, v] : g_cbs) for (size_t i = 0; i < v.size(); ++i) if (v[i] == cb) v.erase(v.begin() + long(i--));
}
S_API void S_CALLTYPE SteamAPI_RegisterCallResult(CCallbackBase* cb, SteamAPICall_t h) { g_calls[h] = cb; }
S_API void S_CALLTYPE SteamAPI_UnregisterCallResult(CCallbackBase*, SteamAPICall_t h) { g_calls.erase(h); }
S_API void S_CALLTYPE SteamAPI_RunCallbacks() {
    std::vector<Ev> q; q.swap(g_queue);
    for (auto& e : q) {
        if (e.call) { auto it = g_calls.find(e.call); if (it != g_calls.end()) { CCallbackBase* cb = it->second; g_calls.erase(it); cb->Run(e.data.data(), false, e.call); } }
        else { auto v = g_cbs[e.id]; for (auto* cb : v) cb->Run(e.data.data()); }
    }
}
// test controls
S_API void fake_overlay(bool on) { GameOverlayActivated_t a{}; a.m_bActive = on; queueCb(a); }
S_API void fake_completePurchase() { SteamItemDetails_t d{}; d.m_itemId = g_nextItem++; d.m_iDefinition = g_pendingDef; d.m_unQuantity = 1; g_items.push_back(d); }
S_API void fake_buyDlc() { g_dlc.insert(g_storeApp); DlcInstalled_t d{}; d.m_nAppID = g_storeApp; queueCb(d); }
S_API int fake_itemCount() { return int(g_items.size()); }
S_API void fake_addItem(int def) { SteamItemDetails_t d{}; d.m_itemId = g_nextItem++; d.m_iDefinition = def; d.m_unQuantity = 1; g_items.push_back(d); }
}
