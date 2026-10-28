-- Meta-game layer for UI scripts. The client runs it before the game's ui/main.lua.
--   ui       the game's screens (ui/screens/<name>.rml): navigation stack, popups, hooks, data model "meta"
--   profile  persistent player data: defaults in ui/profile.json, saved through storage
--   shop     catalog ui/shop.json: items for profile currencies, store products, rewarded ads
--   game.on(event, fn) / game.off(event, fn): screen, open, close, matchEnd, language, purchase, profile, shop, update, back
-- Reference: docs/ui-scripting.md. Games extend these tables from ui/main.lua; don't edit this file.

local ctx = rmlui.contexts["main"]

-- ------------------------------------------------------------------ events
local handlers = {}

function game.on(event, fn)
    handlers[event] = handlers[event] or {}
    table.insert(handlers[event], fn)
    return fn
end

function game.off(event, fn)
    local list = handlers[event]
    if not list then return end
    for i = #list, 1, -1 do if list[i] == fn then table.remove(list, i) end end
end

function game._emit(event, data)
    local list = handlers[event]
    if not list then return end
    for _, fn in ipairs({table.unpack(list)}) do   -- a copy: handlers may unsubscribe themselves
        local ok, err = pcall(fn, data)
        if not ok then print("event " .. event .. ": " .. tostring(err)) end
    end
end

function game._tick(dt)
    if handlers.update then game._emit("update", dt) end
end

local function deepcopy(v)
    if type(v) ~= "table" then return v end
    local t = {}
    for k, x in pairs(v) do t[k] = deepcopy(x) end
    return t
end

-- "@key" -> localized text; anything else as is
local function text(s)
    if type(s) == "string" and s:sub(1, 1) == "@" then return game.tr(s:sub(2)) end
    return s
end

-- ------------------------------------------------------------------ profile
profile = {}
local defaults = game.json("ui/profile.json") or {}
local data
local model   -- data model "meta" (below)

local function fill(dst, src)
    for k, v in pairs(src) do
        if dst[k] == nil then dst[k] = deepcopy(v)
        elseif type(v) == "table" and type(dst[k]) == "table" then fill(dst[k], v) end
    end
end

-- "a.b.c" paths into the profile table
local function lookup(path, create)
    local t, last = data, nil
    local parts = {}
    for p in tostring(path):gmatch("[^%.]+") do parts[#parts + 1] = p end
    for i = 1, #parts - 1 do
        local k = parts[i]
        if type(t[k]) ~= "table" then
            if not create then return nil end
            t[k] = {}
        end
        t = t[k]
    end
    last = parts[#parts]
    return t, last
end

local function changed()
    if model then model.profile = data end
    if not game.preview() then storage.save("profile", data) end
    game._emit("profile", data)
end

function profile.load()
    data = storage.load("profile") or {}
    fill(data, defaults)
    profile.data = data
end

function profile.get(path, default)
    local t, k = lookup(path, false)
    if t == nil or t[k] == nil then return default end
    return t[k]
end

function profile.set(path, value)
    local t, k = lookup(path, true)
    t[k] = value
    changed()
    return value
end

function profile.add(path, n)
    local t, k = lookup(path, true)
    t[k] = (tonumber(t[k]) or 0) + (n or 1)
    changed()
    return t[k]
end

-- profile.can({gold = 100, gems = 5}) -> true | false, first missing key
function profile.can(costs)
    for k, n in pairs(costs or {}) do
        if (tonumber(profile.get(k)) or 0) < n then return false, k end
    end
    return true
end

function profile.spend(costs)
    local ok, missing = profile.can(costs)
    if not ok then return false, missing end
    for k, n in pairs(costs or {}) do
        local t, key = lookup(k, true)
        t[key] = (tonumber(t[key]) or 0) - n
    end
    changed()
    return true
end

-- profile.give({gems = 10, ["skins.red"] = true}): numbers add, other values are set
function profile.give(gifts)
    for k, v in pairs(gifts or {}) do
        local t, key = lookup(k, true)
        if type(v) == "number" then t[key] = (tonumber(t[key]) or 0) + v else t[key] = deepcopy(v) end
    end
    changed()
end

local function merge(dst, src)
    for k, v in pairs(src) do
        if type(v) == "table" and type(dst[k]) == "table" and #v == 0 then merge(dst[k], v) else dst[k] = deepcopy(v) end
    end
end

function profile.merge(tbl) merge(data, tbl or {}); changed() end
function profile.reset() data = deepcopy(defaults); profile.data = data; changed() end
function profile.save() if not game.preview() then storage.save("profile", data) end end

profile.load()

-- ------------------------------------------------------------------ shop
shop = {}
local catalog = game.json("ui/shop.json") or {}
local byId = {}
for _, list in ipairs({catalog.items or {}, catalog.products or {}, catalog.rewarded or {}}) do
    for _, it in ipairs(list) do byId[it.id] = it end
end

local function priceText(price)
    local parts = {}
    local keys = {}
    for k in pairs(price or {}) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do
        if game.hasText("price." .. k) then   -- "price.gems": plural forms with {count}
            parts[#parts + 1] = game.tr("price." .. k, {count = price[k]})
        else
            local name = game.hasText("currency." .. k) and game.tr("currency." .. k) or k
            parts[#parts + 1] = tostring(price[k]) .. " " .. name
        end
    end
    return table.concat(parts, "  ")
end

local function owned(it)
    if not it.once then return false end
    for k, v in pairs(it.give or {}) do
        if type(v) ~= "number" and profile.get(k) == nil then return false end
    end
    return next(it.give or {}) ~= nil
end

local storePrices = {}   -- product id -> localized price from the store

-- Rows for screens: {id, name, description, icon, price, priceText, owned, affordable, kind = item|product|rewarded, ready}
function shop.list(kind)
    local out = {}
    local function add(list, k)
        for _, it in ipairs(list or {}) do
            local row = {id = it.id, kind = k, name = text(it.name or it.id), description = text(it.description or ""),
                         icon = it.icon or "", price = it.price or {}, give = it.give or {}}
            if k == "item" then
                row.priceText = priceText(it.price)
                row.owned = owned(it)
                row.affordable = profile.can(it.price)
            elseif k == "product" then
                row.priceText = storePrices[it.id] or (store.available() and "…" or "—")
                row.owned = owned(it)
                row.affordable = store.available()
            else
                row.priceText = text(it.label or "@shop.watch")
                local left = shop.cooldown(it.id)
                row.ready = left <= 0
                row.affordable = row.ready and ads.available()
                if left > 0 then row.priceText = game.tr("shop.wait", {minutes = math.ceil(left / 60)}) end
            end
            out[#out + 1] = row
        end
    end
    if kind == nil or kind == "item" then add(catalog.items, "item") end
    if kind == nil or kind == "product" then add(catalog.products, "product") end
    if kind == nil or kind == "rewarded" then add(catalog.rewarded, "rewarded") end
    return out
end

local function refreshShop()
    if model then model.shop = shop.list() end
end

-- In-game purchase for profile currencies -> true | false, reason ("unknown" | "owned" | "price")
function shop.buy(id)
    local it = byId[id]
    if not it or not it.price then return false, "unknown" end
    if owned(it) then return false, "owned" end
    local ok = profile.spend(it.price)
    if not ok then game._emit("shop", {id = id, ok = false, reason = "price"}); return false, "price" end
    profile.give(it.give)
    game._emit("shop", {id = id, ok = true})
    return true
end

-- Real money (store product): grants `give` once per purchase token, then finishes the purchase.
local function grantPurchase(p)
    if p.state ~= "purchased" then return false end
    local tokens = profile.get("_tokens") or {}
    if not tokens[p.token] then
        tokens[p.token] = true
        local it = byId[p.product]
        data._tokens = tokens
        if it then profile.give(it.give) else changed() end
    end
    store.finish(p.token)
    game._emit("shop", {id = p.product, ok = true, real = true})
    return true
end

function shop.buyReal(id, done)
    if game.preview() then if done then done(false, "preview") end return end
    store.buy(id, function(p)
        local ok = grantPurchase(p)
        if done then done(ok, p.state, p.error) end
    end)
end

game.on("purchase", function(p) grantPurchase(p) end)   -- pending payments, purchases after a restart

function shop.restore(done)
    store.restore(function(list)
        for _, p in ipairs(list or {}) do grantPurchase(p) end
        if done then done(true) end
    end)
end

-- Rewarded ads: catalog.rewarded = [{id, give, cooldown (s)}]
function shop.cooldown(id)
    local it = byId[id]
    local last = profile.get("_watched." .. tostring(id))
    if not it or not last or not it.cooldown then return 0 end
    return math.max(0, last + it.cooldown - os.time())
end

function shop.watch(id, done)
    local it = byId[id]
    if not it or shop.cooldown(id) > 0 or game.preview() then if done then done(false) end return end
    ads.show("rewarded", function(r)
        if r.rewarded then
            local w = profile.get("_watched") or {}
            w[id] = os.time()
            data._watched = w
            profile.give(it.give)
        end
        if done then done(r.rewarded) end
    end, id)
end

function shop.loadPrices()
    if not store.available() or not catalog.products then return end
    store.products(function(list)
        for _, p in ipairs(list or {}) do storePrices[p.id] = p.price end
        refreshShop()
    end)
end

game.on("profile", refreshShop)

-- ------------------------------------------------------------------ data model "meta"
-- Screens bind it with <div class="root" data-model="meta">: {{profile.gold}}, data-for="it : shop",
-- {{params.title}}, {{screen}}, {{state.x}}; data-event-click="call('shop.buy', it.id)".
local function resolve(name)
    local f = _G
    for part in tostring(name):gmatch("[^%.]+") do
        if type(f) ~= "table" then return nil end
        f = f[part]
    end
    return f
end

model = ctx:OpenDataModel("meta", {
    profile = data,
    params = {},
    screen = "",
    shop = shop.list(),
    state = {},                       -- free for the game: ui.set("state", {...}) or ui.set(key, value)
    game = {name = game.name(), maps = game.maps(), language = game.language(), touch = game.touch()},
    -- <div class="screen" data-attr-style="safeStyle">: content clear of the notch / home indicator / banner
    safe = game.safeArea(),
    safeStyle = "",
    -- call('shop.buy', it.id): any global function by dotted name
    call = function(event, name, ...)
        local f = resolve(name)
        if type(f) ~= "function" then print("call: no function " .. tostring(name)); return end
        f(...)
    end,
    open = function(event, name, ...) ui.open(name, ...) end,
    back = function(event) ui.back() end,
})

local function updateSafe()
    local s = game.safeArea()
    model.safe = s
    model.safeStyle = string.format("padding: %ddp %ddp %ddp %ddp;", math.floor(s.top + 0.5), math.floor(s.right + 0.5),
                                    math.floor(s.bottom + 0.5), math.floor(s.left + 0.5))
end
updateSafe()
game.on("resize", updateSafe)

-- ------------------------------------------------------------------ screens
ui = {}
local docs, shown, hooks = {}, {}, {}
local stack, params = {}, {}
local popups = {}            -- open popups, in order
local active = true          -- screens show on the engine's menu screen only
local preview = false

local function call(fn, ...)
    if not fn then return end
    local ok, err = pcall(fn, ...)
    if not ok then print("ui: " .. tostring(err)) end
end

local function load(name, p)
    if p ~= nil then params[name] = p; model.params = p end   -- bindings read params while the document loads
    local d = docs[name]
    if d then return d end
    d = ctx:LoadDocument("engine/ui/screens/" .. name .. ".rml")   -- the game's ui/screens/<name>.rml
    if not d then error("ui: no screen '" .. tostring(name) .. "' (ui/screens/" .. tostring(name) .. ".rml)", 3) end
    docs[name] = d
    return d
end

local function isPopup(name)
    for _, p in ipairs(popups) do if p == name then return true end end
    return false
end

local function apply()
    local top = stack[#stack]
    for name, d in pairs(docs) do
        local want = active and (name == top or isPopup(name))
        if want ~= (shown[name] or false) then
            if want then d:Show() else d:Hide() end
            shown[name] = want
        end
    end
    if active then for _, p in ipairs(popups) do docs[p]:PullToFront() end end
end

local function current() return popups[#popups] or stack[#stack] end

local function enter(name)
    model.params = params[name] or {}
    model.screen = name
    apply()
    call(hooks[name] and hooks[name].open, params[name] or {}, docs[name])
    game._emit("open", {screen = name, params = params[name]})
end

local function leave(name)
    call(hooks[name] and hooks[name].close, docs[name])
    game._emit("close", {screen = name})
end

-- ui.screen(name, {open = fn(params, doc), close = fn(doc), back = fn() -> true when handled})
function ui.screen(name, h) hooks[name] = h; return h end

function ui.open(name, p)
    p = p or {}
    load(name, p)
    if stack[#stack] then leave(stack[#stack]) end
    stack[#stack + 1] = name
    params[name] = p or {}
    enter(name)
end

function ui.replace(name, p)
    p = p or {}
    load(name, p)
    local cur = table.remove(stack)
    if cur then leave(cur) end
    stack[#stack + 1] = name
    params[name] = p or {}
    enter(name)
end

-- Clears the history: the screen becomes the root (home after a match, after a tutorial)
function ui.reset(name, p)
    p = p or {}
    load(name, p)
    if stack[#stack] then leave(stack[#stack]) end
    stack = {name}
    params[name] = p or {}
    enter(name)
end

function ui.popup(name, p)
    p = p or {}
    load(name, p)
    if isPopup(name) then return end
    popups[#popups + 1] = name
    params[name] = p or {}
    enter(name)
end

function ui.closePopup(name)
    name = name or popups[#popups]
    for i = #popups, 1, -1 do
        if popups[i] == name then
            table.remove(popups, i)
            leave(name)
            local top = current()
            if top then model.params = params[top] or {}; model.screen = top end
            apply()
            return true
        end
    end
    return false
end

-- Back button / Esc: the screen's back hook, then popups, then the stack -> true when something closed
function ui.back()
    local cur = current()
    if cur and hooks[cur] and hooks[cur].back then
        local ok, handled = pcall(hooks[cur].back)
        if ok and handled then return true end
    end
    if #popups > 0 then return ui.closePopup() end
    if #stack > 1 then
        leave(table.remove(stack))
        enter(stack[#stack])
        return true
    end
    return false
end

function ui.current() return current() end
function ui.root() return stack[1] end
function ui.params(name) return params[name or current()] or {} end
function ui.document(name) return docs[name or current()] end
function ui.isOpen(name) return stack[#stack] == name or isPopup(name) end

-- data model values: ui.set("state", {...}); ui.set("level", 3) -> {{level}} in screens
function ui.set(key, value) model[key] = value; return value end
function ui.get(key) return model[key] end
function ui.refresh(key) model[key] = model[key] end

-- Back to the first screen of ui/screens.json ("start"), clearing the history
function ui.home(p)
    local cfg = game.json("ui/screens.json") or {}
    ui.reset(cfg.start or stack[1], p)
end

-- The game's screens replace the engine main menu: ui.start("home")
function ui.start(name, p)
    game.engineMenu(false)
    ui.reset(name, p)
end

-- MCP preview: one screen on its own (stays on the menu screen, nothing is saved, no matches start)
function ui.preview(name, p)
    preview = true
    game.engineMenu(false)
    local cfg = game.json("ui/screens.json") or {}
    local kind = (cfg.screens and cfg.screens[name] and cfg.screens[name].popup) and "popup" or "screen"
    if kind == "popup" and cfg.start and cfg.start ~= name then ui.reset(cfg.start); ui.popup(name, p)
    else ui.reset(name, p) end
end

-- Called by the client after ui/main.lua: ui/screens.json {"start": "home"} opens the game's first screen.
function ui.boot(isPreview)
    if isPreview or #stack > 0 then return end
    local cfg = game.json("ui/screens.json")
    if cfg and cfg.start then ui.start(cfg.start) end
    shop.loadPrices()
end

game.on("screen", function(s)
    active = preview or s == "menu"
    apply()
end)

-- after a match the client returns to the menu: back to the root screen (or cfg.afterMatch)
game.on("matchEnd", function(r)
    local cfg = game.json("ui/screens.json") or {}
    if cfg.afterMatch and #stack > 0 then ui.reset(cfg.afterMatch, {result = r}) end
end)

game.on("back", function() ui.back() end)

-- texts are translated when a document loads ("@key"): reload the open screens in the new language
game.on("language", function()
    model.game = {name = game.name(), maps = game.maps(), language = game.language(), touch = game.touch()}
    for name, d in pairs(docs) do d:Close() end
    docs, shown = {}, {}
    for _, n in ipairs(stack) do load(n) end
    for _, n in ipairs(popups) do load(n) end
    refreshShop()
    local top = current()
    if top then enter(top) end
end)
