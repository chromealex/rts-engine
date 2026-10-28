// NativeActivity + on-screen keyboard. The engine runs in native code (android_native_app_glue);
// this class only adds what NativeActivity lacks: an IME connection that delivers committed text in
// any language (NativeActivity's raw key events lose non-ASCII characters) and the keyboard height.
// Everything here runs on the UI thread; the native side queues the calls for the game thread.
package com.rtsengine.platform;

import android.app.NativeActivity;
import android.content.Context;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

public class RtsActivity extends NativeActivity {
    static final int KEY_BACKSPACE = 0, KEY_ENTER = 1;

    // The natives below live in libmain.so. NativeActivity dlopens it itself, which doesn't make
    // its JNI symbols visible to this class, and insets can arrive before android_main starts.
    static { System.loadLibrary("main"); }

    static native void nativeText(String text);
    static native void nativeKey(int key);
    static native void nativeKeyboard(int heightPx);
    static native void nativeTextInputEnded();

    private TextSink sink;
    private boolean active;     // the game asked for text input
    private int keyboardPx;     // last reported keyboard height

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        sink = new TextSink(this);
        addContentView(sink, new ViewGroup.LayoutParams(1, 1));
        final View decor = getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            decor.setOnApplyWindowInsetsListener((v, insets) -> {
                onKeyboardHeight(insets.getInsets(WindowInsets.Type.ime()).bottom);
                return v.onApplyWindowInsets(insets);
            });
        } else {
            decor.getViewTreeObserver().addOnGlobalLayoutListener(() -> {
                Rect r = new Rect();
                decor.getWindowVisibleDisplayFrame(r);
                int h = decor.getRootView().getHeight() - r.bottom;
                onKeyboardHeight(h > decor.getRootView().getHeight() / 8 ? h : 0); // ignore nav bar
            });
        }
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
