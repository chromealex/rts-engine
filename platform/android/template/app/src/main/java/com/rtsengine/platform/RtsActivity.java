// NativeActivity + on-screen keyboard. The engine runs in native code (android_native_app_glue);
// this class only adds what NativeActivity lacks: an IME connection that delivers committed text in
// any language (NativeActivity's raw key events lose non-ASCII characters) and the keyboard height.
// Everything here runs on the UI thread; the native side queues the calls for the game thread.
package com.rtsengine.platform;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.DisplayCutout;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import org.json.JSONObject;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public class RtsActivity extends NativeActivity {
    static final int KEY_BACKSPACE = 0, KEY_ENTER = 1;

    // The natives below live in libmain.so. NativeActivity dlopens it itself, which doesn't make
    // its JNI symbols visible to this class, and insets can arrive before android_main starts.
    static { System.loadLibrary("main"); }

    static native void nativeText(String text);
    static native void nativeKey(int key);
    static native void nativeKeyboard(int heightPx);
    static native void nativeTextInputEnded();
    static native void nativeSafeArea(int left, int top, int right, int bottom);
    static native void nativePluginReply(long callId, boolean ok, String dataOrError);
    static native void nativePluginEvent(String plugin, String event, String data);

    // ---- plugins: name → class from <meta-data android:name="rts.plugin.NAME" android:value="CLASS"/>
    private final Map<String, String> pluginClasses = new ConcurrentHashMap<>();
    private final Map<String, RtsPlugin> plugins = new ConcurrentHashMap<>();

    private TextSink sink;
    private boolean active;     // the game asked for text input
    private int keyboardPx;     // last reported keyboard height

    @Override
    protected void onCreate(Bundle state) {
        readPluginManifest(); // before super.onCreate: the game thread may ask about plugins right away
        super.onCreate(state);
        sink = new TextSink(this);
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
        if (Build.VERSION.SDK_INT >= 28) {
            // draw under the camera cutout too; safeAreaInsets() tells the UI where it is
            WindowManager.LayoutParams lp = getWindow().getAttributes();
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(lp);
        }
        final View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((v, insets) -> {
            reportSafeArea(insets);
            if (Build.VERSION.SDK_INT >= 30) onKeyboardHeight(insets.getInsets(WindowInsets.Type.ime()).bottom);
            return v.onApplyWindowInsets(insets);
        });
        if (Build.VERSION.SDK_INT < 30) {
            decor.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
                Rect r = new Rect();
                decor.getWindowVisibleDisplayFrame(r);
                int h = decor.getRootView().getHeight() - r.bottom;
                onKeyboardHeight(h > decor.getRootView().getHeight() / 8 ? h : 0); // ignore nav bar
            });
        }
    }

    private void readPluginManifest() {
        try {
            ApplicationInfo ai = getPackageManager().getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA);
            if (ai.metaData == null) return;
            for (String key : ai.metaData.keySet())
                if (key.startsWith("rts.plugin.")) pluginClasses.put(key.substring("rts.plugin.".length()), String.valueOf(ai.metaData.get(key)));
        } catch (Exception e) {
            android.util.Log.e("rts", "plugin manifest", e);
        }
    }

    // Game thread (JNI).
    public boolean pluginAvailable(String name) { return pluginClasses.containsKey(name); }

    // Game thread (JNI): runs the call on the UI thread, where SDKs expect to be used.
    public void pluginCall(final String name, final String method, final String args, final long callId) {
        runOnUiThread(() -> {
            RtsPlugin.Reply reply = new RtsPlugin.Reply(callId);
            try {
                RtsPlugin p = plugin(name);
                if (p == null) { reply.fail("plugin '" + name + "' is not in this build"); return; }
                p.call(method, args == null || args.isEmpty() ? new JSONObject() : new JSONObject(args), reply);
            } catch (Throwable t) {
                android.util.Log.e("rts", "plugin " + name + "." + method, t);
                reply.fail(name + "." + method + ": " + t);
            }
        });
    }

    private RtsPlugin plugin(String name) throws Exception {
        RtsPlugin p = plugins.get(name);
        if (p != null) return p;
        String cls = pluginClasses.get(name);
        if (cls == null) return null;
        p = (RtsPlugin) Class.forName(cls).getDeclaredConstructor().newInstance();
        p.activity = this;
        p.name = name;
        plugins.put(name, p);
        return p;
    }

    @Override protected void onResume() { super.onResume(); for (RtsPlugin p : plugins.values()) p.onResume(); }
    @Override protected void onPause() { for (RtsPlugin p : plugins.values()) p.onPause(); super.onPause(); }
    @Override protected void onDestroy() { for (RtsPlugin p : plugins.values()) p.onDestroy(); super.onDestroy(); }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        for (RtsPlugin p : plugins.values()) p.onNewIntent(intent); // payment deep links (RuStore)
    }
    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        for (RtsPlugin p : plugins.values()) p.onActivityResult(requestCode, resultCode, data);
    }

    // Immersive: system bars hidden, a swipe from the edge shows them for a moment (games standard).
    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (!focused) return;
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    private void reportSafeArea(WindowInsets insets) {
        int l = 0, t = 0, r = 0, b = 0;
        if (Build.VERSION.SDK_INT >= 30) {
            android.graphics.Insets i = insets.getInsets(WindowInsets.Type.displayCutout() | WindowInsets.Type.systemBars());
            l = i.left; t = i.top; r = i.right; b = i.bottom;
        } else if (Build.VERSION.SDK_INT >= 28) {
            DisplayCutout c = insets.getDisplayCutout();
            if (c != null) { l = c.getSafeInsetLeft(); t = c.getSafeInsetTop(); r = c.getSafeInsetRight(); b = c.getSafeInsetBottom(); }
        }
        nativeSafeArea(l, t, r, b);
    }

    // Called from the game thread (JNI). ClipboardManager is safe to use off the UI thread.
    public String clipboardText() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null || !cm.hasPrimaryClip() || cm.getPrimaryClip().getItemCount() == 0) return "";
        CharSequence s = cm.getPrimaryClip().getItemAt(0).coerceToText(this);
        return s != null ? s.toString() : "";
    }
    public void setClipboardText(String s) {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("text", s));
    }

    private void onKeyboardHeight(int px) {
        if (px == keyboardPx) return;
        boolean hidden = keyboardPx > 0 && px == 0;
        keyboardPx = px;
        nativeKeyboard(px);
        if (hidden && active) { // Back / hide key: the user is done with the field
            active = false;
            sink.clearFocus();
            nativeTextInputEnded();
        }
    }

    // Called from the game thread (JNI).
    public void showTextInput(final boolean show) {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
            active = show;
            if (show) {
                sink.setFocusable(true);
                sink.setFocusableInTouchMode(true);
                sink.requestFocus();
                imm.restartInput(sink);
                imm.showSoftInput(sink, 0);
            } else {
                imm.hideSoftInputFromWindow(sink.getWindowToken(), 0);
                sink.clearFocus();
            }
        });
    }

    /** Invisible 1x1 view that owns the IME connection. */
    static final class TextSink extends View {
        TextSink(Context c) { super(c); }

        @Override public boolean onCheckIsTextEditor() { return true; }

        @Override
        public InputConnection onCreateInputConnection(EditorInfo info) {
            info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
            info.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
            return new Connection(this);
        }
    }

    /** Text is kept by the game UI, not here: commits are forwarded, composing text is held until finished. */
    static final class Connection extends BaseInputConnection {
        private String composing = "";
        Connection(View v) { super(v, false); }

        @Override public boolean commitText(CharSequence text, int pos) {
            composing = "";
            if (text.length() > 0) nativeText(text.toString());
            return true;
        }
        @Override public boolean setComposingText(CharSequence text, int pos) {
            composing = text.toString();
            return true;
        }
        @Override public boolean finishComposingText() {
            if (!composing.isEmpty()) nativeText(composing);
            composing = "";
            return true;
        }
        @Override public boolean deleteSurroundingText(int before, int after) {
            for (int i = 0; i < before; ++i) {
                if (!composing.isEmpty()) composing = composing.substring(0, composing.length() - 1);
                else nativeKey(KEY_BACKSPACE);
            }
            return true;
        }
        @Override public boolean sendKeyEvent(KeyEvent e) {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return true;
            switch (e.getKeyCode()) {
                case KeyEvent.KEYCODE_DEL: nativeKey(KEY_BACKSPACE); break;
                case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: nativeKey(KEY_ENTER); break;
                default: {
                    int ch = e.getUnicodeChar();
                    if (ch > 0) nativeText(new String(Character.toChars(ch)));
                }
            }
            return true;
        }
        @Override public boolean performEditorAction(int action) {
            finishComposingText();
            nativeKey(KEY_ENTER);
            return true;
        }
    }
}
