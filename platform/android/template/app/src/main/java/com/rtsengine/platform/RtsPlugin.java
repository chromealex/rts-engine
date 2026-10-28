// Base class of Android plugins (payments, ads, ...). A plugin is declared in AndroidManifest.xml
// (rtsbuild writes it from plugins/<name>/plugin.json):
//   <meta-data android:name="rts.plugin.yandex-ads" android:value="com.rtsengine.plugins.YandexAdsPlugin"/>
// and created on first use. call() runs on the UI thread; answer through the Reply exactly once,
// now or later from any thread. Game-side API: engine/platform/Plugins.h.
package com.rtsengine.platform;

import android.content.Intent;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.concurrent.atomic.AtomicBoolean;

public abstract class RtsPlugin {
    protected RtsActivity activity;
    protected String name;

    /** method "init" comes first, with the plugin's config from game.json. */
    public abstract void call(String method, JSONObject args, Reply reply) throws Exception;

    public void onResume() {}
    public void onPause() {}
    public void onDestroy() {}
    public void onNewIntent(Intent intent) {}
    public void onActivityResult(int requestCode, int resultCode, Intent data) {}

    /** Sends an event to the game (any thread). "app.pause"/"app.resume" pause and resume the game. */
    protected final void emit(String event, Object data) {
        RtsActivity.nativePluginEvent(name, event, toJson(data));
    }

    static String toJson(Object v) {
        if (v == null) return "null";
        if (v instanceof JSONObject || v instanceof JSONArray) return v.toString();
        if (v instanceof String) return JSONObject.quote((String) v);
        return String.valueOf(v); // numbers, booleans
    }

    public static final class Reply {
        private final long id;
        private final AtomicBoolean done = new AtomicBoolean(false);
        Reply(long id) { this.id = id; }
        public void ok(Object data) { if (done.compareAndSet(false, true)) RtsActivity.nativePluginReply(id, true, toJson(data)); }
        public void ok() { ok(new JSONObject()); }
        /** "cancelled" means the user backed out (not an error to show). */
        public void fail(String error) { if (done.compareAndSet(false, true)) RtsActivity.nativePluginReply(id, false, error == null ? "error" : error); }
    }
}
