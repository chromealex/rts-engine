// Yandex Games SDK → the engine's "store" and "ads" protocols (docs/plugins.md).
// Moderation requirements handled here: LoadingAPI.ready() when the engine reports the first
// interactive frame; game_api_pause / game_api_resume pause the game (and its audio) through the
// engine's "app.pause"/"app.resume"; purchases are consumed only after the game granted them
// (store::finish), and restore() returns unprocessed ones on every launch.
(function () {
  "use strict";
  var NAME = "yandex-games";
  var cfg = {};
  var ysdk = null, payments = null;
  var emit = function (ev, data) { window.RTSPlugins.emit(NAME, ev, data || {}); };

  function sdk() {
    if (!ysdk) {
      ysdk = window.YaGames
        ? window.YaGames.init()
        : Promise.reject(new Error("Yandex Games SDK is not loaded (/sdk.js): run on Yandex Games or via `npx @yandex-games/sdk-dev-proxy`"));
    }
    return ysdk;
  }
  function pay() {
    if (!payments) payments = sdk().then(function (y) { return y.getPayments({ signed: !!cfg.signed }); });
    return payments;
  }
  function purchaseJson(p, state) {
    var receipt = {};
    if (p.signature) receipt.signature = p.signature;
    if (p.developerPayload) receipt.developerPayload = p.developerPayload;
    return { productId: p.productID, state: state, token: p.purchaseToken || "", orderId: p.purchaseToken || "", receipt: receipt };
  }
  function message(e) { return e && e.message ? e.message : String(e); }

  window.RTSPlugins.register(NAME, {
    init: function (config) {
      cfg = config || {};
      return sdk().then(function (y) {
        y.on("game_api_pause", function () { emit("app.pause"); });
        y.on("game_api_resume", function () { emit("app.resume"); });
        return { lang: y.environment && y.environment.i18n ? y.environment.i18n.lang : "" };
      });
    },
    lifecycle: function (a) {
      if (a.event !== "ready") return null;
      return sdk().then(function (y) {
        if (y.features && y.features.LoadingAPI) y.features.LoadingAPI.ready();
        return null;
      });
    },

    // ---- store
    products: function (args) {
      var want = {};
      (args.products || []).forEach(function (p) { want[p.id] = p.type || "consumable"; });
      return pay().then(function (p) { return p.getCatalog(); }).then(function (catalog) {
        return catalog.filter(function (c) { return !args.products || args.products.length === 0 || c.id in want; }).map(function (c) {
          var value = parseFloat(c.priceValue);
          return { id: c.id, title: c.title, description: c.description, price: c.price,
                   priceMicros: isNaN(value) ? 0 : Math.round(value * 1e6), currency: c.priceCurrencyCode, type: want[c.id] || "consumable" };
        });
      });
    },
    purchase: function (args) {
      return pay().then(function (p) { return p.purchase({ id: args.id }); }).then(
        function (res) { return purchaseJson(res, "purchased"); },
        function (e) {
          // the SDK rejects when the dialog is closed; other failures carry a message
          var m = message(e);
          throw new Error(/close|cancel/i.test(m) ? "cancelled" : m);
        });
    },
    restore: function () {
      return pay().then(function (p) { return p.getPurchases(); }).then(function (list) {
        return Array.prototype.map.call(list, function (p) { return purchaseJson(p, "restored"); });
      });
    },
    finish: function (args) {
      if (args.consumable === false) return {}; // non-consumables stay in getPurchases() = owned
      return pay().then(function (p) { return p.consumePurchase(args.token); }).then(function () { return {}; });
    },

    // ---- ads (frequency is decided by the platform; the SDK pauses the game via game_api_pause)
    load: function (args) {
      emit("loaded", { format: args.format }); // no preloading in this SDK
      return {};
    },
    show: function (args) {
      return sdk().then(function (y) {
        return new Promise(function (resolve) {
          var format = args.format, rewarded = false;
          var cbs = {
            onOpen: function () { emit("opened", { format: format }); },
            onClose: function (wasShown) {
              emit("closed", { format: format });
              resolve({ shown: format === "rewarded" ? true : !!wasShown, rewarded: rewarded,
                        error: wasShown === false ? "not shown (frequency limit)" : undefined });
            },
            onError: function (e) { resolve({ shown: false, rewarded: false, error: message(e) }); }
          };
          if (format === "rewarded") {
            cbs.onRewarded = function () { rewarded = true; emit("rewarded", { format: format }); };
            y.adv.showRewardedVideo({ callbacks: cbs });
          } else {
            y.adv.showFullscreenAdv({ callbacks: cbs });
          }
        });
      });
    },
    banner: function (args) {
      return sdk().then(function (y) { return args.show ? y.adv.showBannerAdv() : y.adv.hideBannerAdv(); })
        .then(function (st) { return { showing: !!(st && st.stickyAdvIsShowing), reason: st && st.reason }; });
    },
    consent: function () { return {}; }
  });
})();
