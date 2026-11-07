package org.ra3native;

import android.app.NativeActivity;
import android.content.Context;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;

// NativeActivity cannot show a soft keyboard on its own (its view is not a text editor), so a tiny invisible EditText provides the
// input connection and forwards every typed character / key to the native side.
public class MainActivity extends NativeActivity {
    private EditText edit;
    private boolean shown;

    // registered from native code (RegisterNatives)
    public static native void nativeChar(int codeUnit);
    public static native void nativeKey(int androidKey, boolean down);

    public void toggleKeyboard() {
        runOnUiThread(new Runnable() {
            public void run() {
                InputMethodManager imm = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (edit == null) {
                    edit = new EditText(MainActivity.this);
                    edit.setAlpha(0.01f);
                    edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
                    edit.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN);
                    edit.addTextChangedListener(new TextWatcher() {
                        public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
                        public void onTextChanged(CharSequence s, int start, int before, int count) {
                            for (int i = start; i < start + count && i < s.length(); i++) nativeChar(s.charAt(i));
                        }
                        public void afterTextChanged(Editable e) { if (e.length() > 0) e.clear(); }
                    });
                    edit.setOnKeyListener(new View.OnKeyListener() {
                        public boolean onKey(View v, int keyCode, KeyEvent ev) {
                            if (keyCode == KeyEvent.KEYCODE_BACK) return false;
                            if (ev.getAction() == KeyEvent.ACTION_DOWN || ev.getAction() == KeyEvent.ACTION_UP) nativeKey(keyCode, ev.getAction() == KeyEvent.ACTION_DOWN);
                            return true;
                        }
                    });
                    edit.setOnEditorActionListener(new android.widget.TextView.OnEditorActionListener() {
                        public boolean onEditorAction(android.widget.TextView v, int actionId, KeyEvent ev) {
                            nativeKey(KeyEvent.KEYCODE_ENTER, true);
                            nativeKey(KeyEvent.KEYCODE_ENTER, false);
                            return true;
                        }
                    });
                    addContentView(edit, new FrameLayout.LayoutParams(8, 8));
                }
                if (!shown) {
                    edit.setVisibility(View.VISIBLE);
                    edit.requestFocus();
                    imm.showSoftInput(edit, InputMethodManager.SHOW_FORCED);
                    shown = true;
                } else {
                    imm.hideSoftInputFromWindow(edit.getWindowToken(), 0);
                    edit.clearFocus();
                    shown = false;
                }
            }
        });
    }
}
