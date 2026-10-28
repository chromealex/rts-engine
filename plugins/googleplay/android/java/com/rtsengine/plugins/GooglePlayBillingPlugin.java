// Google Play Billing Library 9 → the engine's "store" protocol (docs/plugins.md).
package com.rtsengine.plugins;

import com.android.billingclient.api.AcknowledgePurchaseParams;
import com.android.billingclient.api.BillingClient;
import com.android.billingclient.api.BillingClientStateListener;
import com.android.billingclient.api.BillingFlowParams;
import com.android.billingclient.api.BillingResult;
import com.android.billingclient.api.ConsumeParams;
import com.android.billingclient.api.PendingPurchasesParams;
import com.android.billingclient.api.ProductDetails;
import com.android.billingclient.api.Purchase;
import com.android.billingclient.api.QueryProductDetailsParams;
import com.android.billingclient.api.QueryPurchasesParams;
import com.rtsengine.platform.RtsPlugin;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BiConsumer;

public class GooglePlayBillingPlugin extends RtsPlugin {
    private BillingClient client;
    private boolean connected;
    private final List<Runnable> waiting = new ArrayList<>();          // calls made before the connection is up
    private final Map<String, ProductDetails> details = new HashMap<>(); // productId → details (needed to launch a purchase)
    private RtsPlugin.Reply purchaseReply;                               // the purchase() in flight
    private String purchaseId;

    @Override
    public void call(String method, JSONObject args, Reply reply) {
        switch (method) {
            case "init": connect(); reply.ok(); return;
            case "products": whenConnected(() -> products(args, reply)); return;
            case "purchase": whenConnected(() -> purchase(args, reply)); return;
            case "restore": whenConnected(() -> restore(reply)); return;
            case "finish": whenConnected(() -> finish(args, reply)); return;
            case "lifecycle": reply.ok(); return;
            default: reply.fail("googleplay: unknown method " + method);
        }
    }

    @Override
    public void onResume() { if (connected) restore(null); } // purchases completed outside the app (pending payments)

    @Override
    public void onDestroy() { if (client != null) client.endConnection(); }

    private void connect() {
        if (client != null) return;
        client = BillingClient.newBuilder(activity)
            .setListener(this::onPurchasesUpdated)
            .enablePendingPurchases(PendingPurchasesParams.newBuilder().enableOneTimeProducts().build())
            .enableAutoServiceReconnection()
            .build();
        client.startConnection(new BillingClientStateListener() {
            @Override public void onBillingSetupFinished(BillingResult r) {
                activity.runOnUiThread(() -> {
                    connected = r.getResponseCode() == BillingClient.BillingResponseCode.OK;
                    List<Runnable> run = new ArrayList<>(waiting);
                    waiting.clear();
                    for (Runnable q : run) q.run(); // they fail cleanly if not connected
                });
            }
            @Override public void onBillingServiceDisconnected() { /* auto reconnection is on */ }
        });
    }

    private void whenConnected(Runnable r) {
        connect();
        if (connected) r.run(); else waiting.add(r);
    }

    private void products(JSONObject args, Reply reply) {
        queryDetails(args, (out, err) -> { if (err == null) reply.ok(out); else reply.fail(err); });
    }

    /** Fetches ProductDetails (cached for purchases); done(array, null) or done(null, error). */
    private void queryDetails(JSONObject args, BiConsumer<JSONArray, String> done) {
        JSONArray list = args.optJSONArray("products");
        List<QueryProductDetailsParams.Product> inapp = new ArrayList<>(), subs = new ArrayList<>();
        for (int i = 0; list != null && i < list.length(); ++i) {
            JSONObject p = list.optJSONObject(i);
            boolean sub = "subscription".equals(p.optString("type"));
            QueryProductDetailsParams.Product q = QueryProductDetailsParams.Product.newBuilder()
                .setProductId(p.optString("id"))
                .setProductType(sub ? BillingClient.ProductType.SUBS : BillingClient.ProductType.INAPP).build();
            (sub ? subs : inapp).add(q);
        }
        JSONArray out = new JSONArray();
        int[] left = { (inapp.isEmpty() ? 0 : 1) + (subs.isEmpty() ? 0 : 1) };
        if (left[0] == 0) { done.accept(out, null); return; }
        for (List<QueryProductDetailsParams.Product> group : Arrays.asList(inapp, subs)) {
            if (group.isEmpty()) continue;
            client.queryProductDetailsAsync(QueryProductDetailsParams.newBuilder().setProductList(group).build(), (br, res) -> {
                activity.runOnUiThread(() -> {
                    if (br.getResponseCode() == BillingClient.BillingResponseCode.OK)
                        for (ProductDetails pd : res.getProductDetailsList()) { details.put(pd.getProductId(), pd); out.put(toJson(pd)); }
                    if (--left[0] == 0) {
                        if (out.length() == 0 && br.getResponseCode() != BillingClient.BillingResponseCode.OK) done.accept(null, error(br));
                        else done.accept(out, null);
                    }
                });
            });
        }
    }

    private void purchase(JSONObject args, Reply reply) {
        if (!connected) { reply.fail("Google Play is not available"); return; }
        String id = args.optString("id");
        ProductDetails pd = details.get(id);
        if (pd == null) { // the game didn't ask for products() first: fetch and retry once
            JSONObject a = new JSONObject();
            try { a.put("products", new JSONArray().put(new JSONObject().put("id", id).put("type", args.optString("type")))); } catch (Exception ignored) {}
            queryDetails(a, (out, err) -> {
                if (details.containsKey(id)) purchase(args, reply); else reply.fail(err != null ? err : "unknown product " + id);
            });
            return;
        }
        BillingFlowParams.ProductDetailsParams.Builder pdp = BillingFlowParams.ProductDetailsParams.newBuilder().setProductDetails(pd);
        if (pd.getSubscriptionOfferDetails() != null && !pd.getSubscriptionOfferDetails().isEmpty())
            pdp.setOfferToken(pd.getSubscriptionOfferDetails().get(0).getOfferToken());
        purchaseReply = reply;
        purchaseId = id;
        BillingResult r = client.launchBillingFlow(activity, BillingFlowParams.newBuilder().setProductDetailsParamsList(Collections.singletonList(pdp.build())).build());
        if (r.getResponseCode() != BillingClient.BillingResponseCode.OK) finishPurchaseReply(null, r);
    }

    private void onPurchasesUpdated(BillingResult r, List<Purchase> purchases) {
        activity.runOnUiThread(() -> {
            if (r.getResponseCode() == BillingClient.BillingResponseCode.OK && purchases != null) {
                for (Purchase p : purchases) {
                    JSONObject j = toJson(p, false);
                    if (purchaseReply != null && p.getProducts().contains(purchaseId)) finishPurchaseReply(j, r);
                    else emit("purchase", j); // pending purchase completed, or bought outside purchase()
                }
            } else {
                finishPurchaseReply(null, r);
            }
        });
    }

    private void finishPurchaseReply(JSONObject purchase, BillingResult r) {
        Reply reply = purchaseReply;
        purchaseReply = null;
        if (reply == null) return;
        if (purchase != null) reply.ok(purchase);
        else if (r.getResponseCode() == BillingClient.BillingResponseCode.USER_CANCELED) reply.fail("cancelled");
        else if (r.getResponseCode() == BillingClient.BillingResponseCode.ITEM_ALREADY_OWNED) reply.fail("already owned: call restore()");
        else reply.fail(error(r));
    }

    private void restore(Reply reply) {
        JSONArray out = new JSONArray();
        int[] left = {2};
        for (String type : new String[] {BillingClient.ProductType.INAPP, BillingClient.ProductType.SUBS}) {
            client.queryPurchasesAsync(QueryPurchasesParams.newBuilder().setProductType(type).build(), (br, list) -> {
                activity.runOnUiThread(() -> {
                    if (br.getResponseCode() == BillingClient.BillingResponseCode.OK && list != null)
                        for (Purchase p : list) {
                            // owned non-consumables / subscriptions, and consumables never consumed
                            if (reply == null) { if (!p.isAcknowledged()) emit("purchase", toJson(p, true)); }
                            else out.put(toJson(p, true));
                        }
                    if (--left[0] == 0 && reply != null) reply.ok(out);
                });
            });
        }
    }

    private void finish(JSONObject args, Reply reply) {
        String token = args.optString("token");
        if (args.optBoolean("consumable", true)) {
            client.consumeAsync(ConsumeParams.newBuilder().setPurchaseToken(token).build(), (br, t) ->
                activity.runOnUiThread(() -> { if (br.getResponseCode() == BillingClient.BillingResponseCode.OK) reply.ok(); else reply.fail(error(br)); }));
        } else {
            client.acknowledgePurchase(AcknowledgePurchaseParams.newBuilder().setPurchaseToken(token).build(), br ->
                activity.runOnUiThread(() -> {
                    int c = br.getResponseCode();
                    if (c == BillingClient.BillingResponseCode.OK) reply.ok(); else reply.fail(error(br));
                }));
        }
    }

    private static JSONObject toJson(ProductDetails pd) {
        JSONObject o = new JSONObject();
        try {
            o.put("id", pd.getProductId()).put("title", pd.getName()).put("description", pd.getDescription());
            ProductDetails.OneTimePurchaseOfferDetails one = pd.getOneTimePurchaseOfferDetails();
            if (one != null) {
                o.put("price", one.getFormattedPrice()).put("priceMicros", one.getPriceAmountMicros()).put("currency", one.getPriceCurrencyCode());
            } else if (pd.getSubscriptionOfferDetails() != null && !pd.getSubscriptionOfferDetails().isEmpty()) {
                List<ProductDetails.PricingPhase> ph = pd.getSubscriptionOfferDetails().get(0).getPricingPhases().getPricingPhaseList();
                ProductDetails.PricingPhase last = ph.get(ph.size() - 1); // the regular price after trials/intro
                o.put("price", last.getFormattedPrice()).put("priceMicros", last.getPriceAmountMicros()).put("currency", last.getPriceCurrencyCode());
                o.put("type", "subscription");
            }
        } catch (Exception ignored) {}
        return o;
    }

    private static JSONObject toJson(Purchase p, boolean restored) {
        JSONObject o = new JSONObject();
        try {
            o.put("productId", p.getProducts().isEmpty() ? "" : p.getProducts().get(0))
             .put("state", p.getPurchaseState() == Purchase.PurchaseState.PURCHASED ? (restored ? "restored" : "purchased") : "pending")
             .put("token", p.getPurchaseToken())
             .put("orderId", p.getOrderId() == null ? "" : p.getOrderId())
             .put("receipt", new JSONObject().put("originalJson", p.getOriginalJson()).put("signature", p.getSignature()));
        } catch (Exception ignored) {}
        return o;
    }

    private static String error(BillingResult r) {
        String m = r.getDebugMessage();
        return "Google Play error " + r.getResponseCode() + (m == null || m.isEmpty() ? "" : ": " + m);
    }
}
