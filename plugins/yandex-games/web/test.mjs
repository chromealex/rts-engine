// Protocol test of yandex-games.js against a fake Yandex Games SDK (node plugins/yandex-games/web/test.mjs).
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';

const events = [];
const consumed = [];
let purchaseOutcome = 'ok';
const fakeSdk = {
  environment: { i18n: { lang: 'ru' } },
  features: { LoadingAPI: { ready() { events.push('ready'); } } },
  on(name, cb) { fakeSdk[name] = cb; },
  adv: {
    showFullscreenAdv({ callbacks }) { callbacks.onOpen(); callbacks.onClose(false); },             // frequency limit
    showRewardedVideo({ callbacks }) { callbacks.onOpen(); callbacks.onRewarded(); callbacks.onClose(true); },
    showBannerAdv: async () => ({ stickyAdvIsShowing: true }),
    hideBannerAdv: async () => ({ stickyAdvIsShowing: false }),
  },
  async getPayments() {
    return {
      getCatalog: async () => [
        { id: 'gems_100', title: 'Gems', description: '100 gems', price: '15 YAN', priceValue: '15', priceCurrencyCode: 'YAN' },
        { id: 'other', title: 'Other', description: '', price: '1 YAN', priceValue: '1', priceCurrencyCode: 'YAN' },
      ],
      purchase: async ({ id }) => {
        if (purchaseOutcome === 'closed') throw new Error('Purchase dialog closed by user');
        return { productID: id, purchaseToken: 'tok-' + id, developerPayload: '' };
      },
      getPurchases: async () => [{ productID: 'no_ads', purchaseToken: 'tok-no_ads' }],
      consumePurchase: async (t) => { consumed.push(t); },
    };
  },
};
const window = {
  YaGames: { init: async () => fakeSdk },
  RTSPlugins: { plugins: {}, register(n, p) { this.plugins[n] = p; }, emit(n, e, d) { events.push(e); } },
};
const src = fs.readFileSync(fileURLToPath(new URL('./yandex-games.js', import.meta.url)), 'utf8');
vm.runInNewContext(src, { window, Promise, Array, Math, parseFloat, isNaN, String, Error });
const p = window.RTSPlugins.plugins['yandex-games'];

const same = (a, b) => assert.equal(JSON.stringify(a), JSON.stringify(b)); // objects come from another VM realm
same(await p.init({}), { lang: 'ru' });
await p.lifecycle({ event: 'ready' });
assert.ok(events.includes('ready'));
fakeSdk.game_api_pause(); fakeSdk.game_api_resume();
assert.ok(events.includes('app.pause') && events.includes('app.resume'));

const cat = await p.products({ products: [{ id: 'gems_100', type: 'consumable' }] });
assert.equal(cat.length, 1);
assert.equal(cat[0].priceMicros, 15000000);
assert.equal(cat[0].currency, 'YAN');

const bought = await p.purchase({ id: 'gems_100' });
assert.equal(bought.state, 'purchased');
assert.equal(bought.token, 'tok-gems_100');
purchaseOutcome = 'closed';
await assert.rejects(p.purchase({ id: 'gems_100' }), /cancelled/);

const restored = await p.restore({});
assert.equal(restored[0].state, 'restored');
await p.finish({ id: 'no_ads', token: 'tok-no_ads', consumable: false });
await p.finish({ id: 'gems_100', token: 'tok-gems_100', consumable: true });
same(consumed, ['tok-gems_100']); // non-consumables are never consumed

const r = await p.show({ format: 'rewarded' });
assert.equal(r.rewarded, true);
const i = await p.show({ format: 'interstitial' });
assert.equal(i.shown, false);
assert.match(i.error, /frequency/);
same(await p.banner({ show: true }), { showing: true });
console.log('yandex-games plugin protocol: OK');
