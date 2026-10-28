// RuStore Pay SDK 11 → the engine's "store" protocol (docs/plugins.md). Signatures checked against
// the SDK's bytecode (ru.rustore.sdk:pay 11.1). The SDK reads console_app_id_value and
// sdk_pay_scheme_value from the manifest (rtsbuild writes them from game.json config).
package com.rtsengine.plugins;

import android.content.Intent;

import com.rtsengine.platform.RtsPlugin;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

import ru.rustore.sdk.pay.RuStorePayClient;
import ru.rustore.sdk.pay.model.AcknowledgementState;
import ru.rustore.sdk.pay.model.PreferredPurchaseType;
import ru.rustore.sdk.pay.model.Product;
import ru.rustore.sdk.pay.model.ProductId;
import ru.rustore.sdk.pay.model.ProductPurchase;
import ru.rustore.sdk.pay.model.ProductPurchaseParams;
import ru.rustore.sdk.pay.model.ProductPurchaseResult;
import ru.rustore.sdk.pay.model.PurchaseId;
import ru.rustore.sdk.pay.model.SdkTheme;

public class RuStorePayPlugin extends RtsPlugin {
    private RuStorePayClient client() { return RuStorePayClient.Companion.getInstance(); }

    @Override
    public void call(String method, JSONObject args, Reply reply) {
        switch (method) {
            case "init":
                // the activity may have been started by the payment deep link
                Intent start = activity.getIntent();
                if (start != null && start.getData() != null) client().getIntentInteractor().proceedIntent(start, SdkTheme.LIGHT);
                reply.ok();
                return;
            case "products": products(args, reply); return;
            case "purchase": purchase(args, reply); return;
            case "restore": restore(reply); return;
            case "finish": finish(args, reply); return;
            case "lifecycle": reply.ok(); return;
            default: reply.fail("rustore: unknown method " + method);
        }
    }

    @Override
    public void onNewIntent(Intent intent) {
        client().getIntentInteractor().proceedIntent(intent, SdkTheme.LIGHT); // back from the payment app
    }

    private void products(JSONObject args, Reply reply) {
        JSONArray list = args.optJSONArray("products");
        List<ProductId> ids = new ArrayList<>();
        for (int i = 0; list != null && i < list.length(); ++i) ids.add(new ProductId(list.optJSONObject(i).optString("id")));
        client().getProductInteractor().getProducts(ids)
            .addOnSuccessListener(products -> {
                JSONArray out = new JSONArray();
                for (Object o : (List<?>) products) {
                    Product p = (Product) o;
                    JSONObject j = new JSONObject();
                    try {
                        j.put("id", p.getProductId().getValue());
                        if (p.getTitle() != null) j.put("title", p.getTitle().getValue());
                        if (p.getDescription() != null) j.put("description", p.getDescription().getValue());
                        if (p.getAmountLabel() != null) j.put("price", p.getAmountLabel().getValue());       // "100.00 руб."
                        if (p.getPrice() != null) j.put("priceMicros", (long) p.getPrice().getValue() * 10000L); // kopecks → micros
                        if (p.getCurrency() != null) j.put("currency", p.getCurrency().getValue());
                        j.put("type", typeName(p.getType() == null ? "" : p.getType().name()));
                    } catch (Exception ignored) {}
                    out.put(j);
                }
                reply.ok(out);
            })
            .addOnFailureListener(t -> reply.fail("RuStore: " + t));
    }

    private void purchase(JSONObject args, Reply reply) {
        String id = args.optString("id");
        ProductPurchaseParams params = new ProductPurchaseParams(new ProductId(id), null, null, null, null, null);
        emit("app.pause", null);
        client().getPurchaseInteractor().purchase(params, PreferredPurchaseType.ONE_STEP, SdkTheme.LIGHT, null)
            .addOnSuccessListener(r -> {
                emit("app.resume", null);
                ProductPurchaseResult res = r;
                JSONObject j = new JSONObject();
                try {
                    j.put("productId", res.getProductId().getValue())
                     .put("state", "purchased")
                     .put("token", res.getPurchaseId().getValue())
                     .put("orderId", res.getOrderId() == null ? "" : res.getOrderId().getValue())
                     .put("receipt", new JSONObject().put("invoiceId", res.getInvoiceId() == null ? "" : res.getInvoiceId().getValue())
                                                     .put("sandbox", res.getSandbox()));
                } catch (Exception ignored) {}
                reply.ok(j);
            })
            .addOnFailureListener(t -> {
                emit("app.resume", null);
                // RuStorePaymentException.ProductPurchaseCancelled: checked by name, the class lives in an internal package
                if (t.getClass().getSimpleName().contains("Cancelled")) reply.fail("cancelled");
                else reply.fail("RuStore: " + t.getMessage());
            });
    }

    private void restore(Reply reply) {
        client().getPurchaseInteractor().getPurchases(null, null, null)
            .addOnSuccessListener(list -> {
                JSONArray out = new JSONArray();
                for (Object o : (List<?>) list) {
                    if (!(o instanceof ProductPurchase)) continue; // subscriptions: BillingSubscription (not mapped yet)
                    ProductPurchase p = (ProductPurchase) o;
                    String status = p.getStatus() == null ? "" : p.getStatus().name();
                    if (!status.equals("CONFIRMED") && !status.equals("PAID")) continue;
                    boolean consumable = p.getProductType() != null && p.getProductType().name().startsWith("CONSUMABLE");
                    // consumables come back only until the game acknowledged them (= granted)
                    if (consumable && p.getAcknowledgementState() != null && p.getAcknowledgementState().name().equals("ACKNOWLEDGED")) continue;
                    JSONObject j = new JSONObject();
                    try {
                        j.put("productId", p.getProductId().getValue()).put("state", "restored").put("token", p.getPurchaseId().getValue())
                         .put("orderId", p.getOrderId() == null ? "" : p.getOrderId().getValue());
                    } catch (Exception ignored) {}
                    out.put(j);
                }
                reply.ok(out);
            })
            .addOnFailureListener(t -> reply.fail("RuStore: " + t.getMessage()));
    }

    // One-step payments need no confirmation; acknowledging marks the item as granted, so restore()
    // stops returning consumables and the RuStore console shows it as delivered.
    private void finish(JSONObject args, Reply reply) {
        client().getPurchaseInteractor().updateAcknowledgementState(new PurchaseId(args.optString("token")), AcknowledgementState.valueOf("ACKNOWLEDGED"), null)
            .addOnSuccessListener(r -> reply.ok())
            .addOnFailureListener(t -> reply.fail("RuStore: " + t.getMessage()));
    }

    private static String typeName(String rustoreType) {
        if (rustoreType.startsWith("NON_CONSUMABLE")) return "nonConsumable";
        if (rustoreType.startsWith("SUBSCRIPTION")) return "subscription";
        return "consumable";
    }
}
