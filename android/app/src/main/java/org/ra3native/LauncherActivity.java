package org.ra3native;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.BufferedReader;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

// Start screen: settings in the style of the game's menus, then launches the game (MainActivity).
// Settings go to <app data>/launcher.cfg, which the native side reads at start.
public class LauncherActivity extends Activity {
    // palette
    static final int RED = 0xffd0161a, RED_DARK = 0xff6e0a0c, RED_GLOW = 0xffff3b30;
    static final int STEEL = 0xff2a2d31, STEEL_LIGHT = 0xff4a4f55, STEEL_EDGE = 0xff7b828a;
    static final int TEXT = 0xffeceae4, TEXT_DIM = 0xff9da3a8, YELLOW = 0xffffc21a;

    SharedPreferences prefs;
    Typeface condensed, condensedBold;
    TextView langValue, modValue, gameText, playButton, warnText, statusText;
    // mods: "<folder>/<file>.skudef" under the Mods folder of the game folder; "" = the game without a mod
    List<String> mods = new ArrayList<>();
    Backdrop backdrop;
    volatile boolean busy;
    List<String> languages = new ArrayList<>();
    String gamePath = "";

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (MainActivity.running) {   // the game is already running in this process: go back to it
            startActivity(new Intent(this, MainActivity.running113 ? MainActivity113.class : MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT));
            finish();
            return;
        }
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        condensed = Typeface.create("sans-serif-condensed", Typeface.NORMAL);
        condensedBold = Typeface.create("sans-serif-condensed", Typeface.BOLD);
        gamePath = prefs.getString("gamepath", "");
        if (gamePath.isEmpty()) gamePath = readText(new File(getFilesDir(), "gamepath.txt")).trim();
        buildUi();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (prefs == null) return;
        refreshGame();
        hideSystemUi();
    }

    void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    int dp(float v) { return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }

    boolean hasStorage() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    // ---------------------------------------------------------------- UI
    void buildUi() {
        FrameLayout root = new FrameLayout(this);
        backdrop = new Backdrop(this);
        root.addView(backdrop, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(dp(28), dp(14), dp(28), dp(22));
        root.addView(row, new FrameLayout.LayoutParams(-1, -1));

        // left: title and main buttons
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        row.addView(left, new LinearLayout.LayoutParams(0, -1, 1f));

        left.addView(label("COMMAND & CONQUER", 11, TEXT_DIM, true, 0.3f));
        TextView title = label("RED ALERT 3", 34, RED, true, 0.04f);
        title.setSingleLine(true);
        title.setShadowLayer(dp(10), 0, dp(2), 0xcc000000);
        left.addView(title);
        TextView sub = label(BuildConfig.STEAM ? "NATIVE  \u2022  ANDROID  \u2022  STEAM" : "NATIVE  \u2022  ANDROID", 10, YELLOW, true, 0.35f);
        sub.setPadding(dp(2), dp(2), 0, dp(12));
        left.addView(sub);

        playButton = menuButton("PLAY", true);
        playButton.setOnClickListener(v -> play());
        playButton.setEnabled(false);
        playButton.setAlpha(0.35f);
        left.addView(playButton, buttonLp());
        TextView folder = menuButton("GAME FOLDER", false);
        folder.setOnClickListener(v -> chooseFolder());
        left.addView(folder, buttonLp());
        TextView exit = menuButton("EXIT", false);
        exit.setOnClickListener(v -> finishAndRemoveTask());
        left.addView(exit, buttonLp());

        View spacer = new View(this);
        left.addView(spacer, new LinearLayout.LayoutParams(1, 0, 1f));
        gameText = label("", 10, TEXT_DIM, false, 0f);
        left.addView(gameText);
        TextView ver = label(versionLabel(), 9, 0xff6b7176, false, 0.2f);
        left.addView(ver);

        // right: settings panel
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(new PanelDrawable());
        panel.setPadding(dp(18), dp(12), dp(18), dp(10));
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(dp(330), -1);
        plp.leftMargin = dp(24);
        row.addView(panel, plp);

        TextView ph = label("OPTIONS", 15, TEXT, true, 0.3f);
        ph.setPadding(0, 0, 0, dp(4));
        panel.addView(ph);
        View rule = new View(this);
        rule.setBackgroundColor(RED);
        panel.addView(rule, new LinearLayout.LayoutParams(-1, dp(2)));

        ScrollView sv = new ScrollView(this);
        LinearLayout opts = new LinearLayout(this);
        opts.setOrientation(LinearLayout.VERTICAL);
        opts.setPadding(0, dp(8), 0, 0);
        sv.addView(opts);
        panel.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1f));

        // language
        LinearLayout lr = optionRow(opts, "INTERFACE LANGUAGE", "Language files found in the game folder");
        LinearLayout sel = new LinearLayout(this);
        sel.setGravity(Gravity.CENTER_VERTICAL);
        TextView prev = arrow("\u25C0"), next = arrow("\u25B6");
        langValue = label("", 12, TEXT, true, 0.12f);
        langValue.setGravity(Gravity.CENTER);
        langValue.setMinWidth(dp(80));
        prev.setOnClickListener(v -> stepLanguage(-1));
        next.setOnClickListener(v -> stepLanguage(1));
        sel.addView(prev); sel.addView(langValue); sel.addView(next);
        lr.addView(sel);

        // mod
        LinearLayout mr = optionRow(opts, "MOD", "Mods in the Mods folder of the game folder (copy them there from Documents/Red Alert 3/Mods)");
        LinearLayout msel = new LinearLayout(this);
        msel.setGravity(Gravity.CENTER_VERTICAL);
        TextView mprev = arrow("\u25C0"), mnext = arrow("\u25B6");
        modValue = label("", 12, TEXT, true, 0.12f);
        modValue.setGravity(Gravity.CENTER);
        modValue.setMinWidth(dp(80));
        modValue.setMaxWidth(dp(220));
        modValue.setSingleLine(true);
        modValue.setEllipsize(android.text.TextUtils.TruncateAt.END);
        mprev.setOnClickListener(v -> stepMod(-1));
        mnext.setOnClickListener(v -> stepMod(1));
        msel.addView(mprev); msel.addView(modValue); msel.addView(mnext);
        mr.addView(msel);
        showMod();

        // HUD size
        LinearLayout ur = optionRow(opts, "HUD SIZE", "Enlarges the minimap, build menu and other corner panels");
        LinearLayout usel = new LinearLayout(this);
        usel.setGravity(Gravity.CENTER_VERTICAL);
        TextView uprev = arrow("\u25C0"), unext = arrow("\u25B6");
        uiValue = label("", 12, TEXT, true, 0.12f);
        uiValue.setGravity(Gravity.CENTER);
        uiValue.setMinWidth(dp(80));
        uprev.setOnClickListener(v -> stepUiScale(-1));
        unext.setOnClickListener(v -> stepUiScale(1));
        usel.addView(uprev); usel.addView(uiValue); usel.addView(unext);
        ur.addView(usel);
        showUiScale();

        stepRow(opts, "BUTTON SIZE", "Size of the on-screen hotkey buttons", "button_size", new int[]{80, 90, 100, 110, 120, 130, 140, 150, 160, 180, 200}, 100);
        stepRow(opts, "BUTTON OPACITY", "How solid the on-screen buttons are (lower: more of the map shows through)", "button_opacity", new int[]{30, 40, 50, 60, 70, 80, 90, 100}, 100);
        stepRow(opts, "CAMERA ROTATION", "How fast a two-finger drag turns the camera", "rotate_speed", new int[]{100, 200, 300, 400, 500, 600, 700, 800, 900, 1000}, 300);
        toggleRow(opts, "DRAG THE MAP", "Swipe moves the map under your finger (off: swipe moves the camera, like the mouse)", "pan_invert", false);
        toggleRow(opts, "CUTSCENES", "Intro, briefings and campaign videos", "cutscenes", true);
        toggleRow(opts, "WIDE SCREEN", "The game fills the whole screen instead of 16:9 (more of the map, buttons on top)", "wide_screen", false);
        toggleRow(opts, "FAST SHADERS", "Lower precision in pixel shaders: faster on Mali and weaker GPUs, rare visual glitches", "fast_shaders", false);
        toggleRow(opts, "LOG FILE", "Writes /sdcard/" + (BuildConfig.STEAM ? "ra3_native_steam.log" : "ra3_native.log") + " for bug reports", "log", logDefault());

        warnText = label("", 10, YELLOW, false, 0f);
        warnText.setPadding(0, dp(8), 0, 0);
        opts.addView(warnText);

        statusText = label("", 10, YELLOW, true, 0.15f);
        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.LEFT);
        slp.leftMargin = dp(28); slp.bottomMargin = dp(14);
        root.addView(statusText, slp);

        setContentView(root);
    }

    void status(final String text, final int color) {
        runOnUiThread(() -> { statusText.setText(text); statusText.setTextColor(color); });
    }

    LinearLayout.LayoutParams buttonLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(230), dp(40));
        lp.bottomMargin = dp(9);
        return lp;
    }

    TextView label(String s, float sp, int color, boolean bold, float spacing) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(bold ? condensedBold : condensed);
        t.setLetterSpacing(spacing);
        t.setIncludeFontPadding(false);
        return t;
    }

    TextView arrow(String s) {
        TextView t = label(s, 12, YELLOW, true, 0f);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(8), dp(4), dp(8), dp(4));
        t.setBackground(bevel(STEEL_LIGHT, STEEL, STEEL_EDGE, 0xff5a6066, 0xff33373b));
        return t;
    }

    // angled red/steel button, as in the game's menus
    TextView menuButton(String s, boolean primary) {
        TextView t = label(s, 15, TEXT, true, 0.2f);
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setPadding(dp(18), 0, dp(14), 0);
        t.setBackground(new ButtonDrawable(primary));
        t.setClickable(true);
        return t;
    }

    StateListDrawable bevel(int top, int bottom, int edge, int topP, int bottomP) {
        GradientDrawable n = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{top, bottom});
        n.setStroke(dp(1), edge);
        GradientDrawable p = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[]{topP, bottomP});
        p.setStroke(dp(1), RED_GLOW);
        StateListDrawable d = new StateListDrawable();
        d.addState(new int[]{android.R.attr.state_pressed}, p);
        d.addState(new int[]{}, n);
        return d;
    }

    LinearLayout optionRow(LinearLayout parent, String name, String hint) {
        LinearLayout r = new LinearLayout(this);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(6), 0, dp(6));
        LinearLayout txt = new LinearLayout(this);
        txt.setOrientation(LinearLayout.VERTICAL);
        txt.addView(label(name, 12, TEXT, true, 0.1f));
        TextView h = label(hint, 9.5f, TEXT_DIM, false, 0f);
        h.setPadding(0, dp(2), dp(8), 0);
        txt.addView(h);
        r.addView(txt, new LinearLayout.LayoutParams(0, -2, 1f));
        parent.addView(r);
        View sep = new View(this);
        sep.setBackgroundColor(0x33ffffff);
        parent.addView(sep, new LinearLayout.LayoutParams(-1, 1));
        return r;
    }

    // an option with a value list and arrows (percent values)
    void stepRow(LinearLayout parent, String name, String hint, final String key, final int[] values, final int def) {
        LinearLayout r = optionRow(parent, name, hint);
        LinearLayout sel = new LinearLayout(this);
        sel.setGravity(Gravity.CENTER_VERTICAL);
        TextView prev = arrow("\u25C0"), next = arrow("\u25B6");
        final TextView value = label("", 12, TEXT, true, 0.12f);
        value.setGravity(Gravity.CENTER);
        value.setMinWidth(dp(80));
        final Runnable show = () -> value.setText(prefs.getInt(key, def) + "%");
        final java.util.function.IntConsumer step = d -> {
            int cur = prefs.getInt(key, def), i = 0;
            for (int k = 0; k < values.length; k++) if (values[k] <= cur) i = k;
            i = Math.max(0, Math.min(values.length - 1, i + d));
            prefs.edit().putInt(key, values[i]).apply();
            show.run();
        };
        prev.setOnClickListener(v -> step.accept(-1));
        next.setOnClickListener(v -> step.accept(1));
        sel.addView(prev); sel.addView(value); sel.addView(next);
        r.addView(sel);
        show.run();
    }

    void toggleRow(LinearLayout parent, String name, String hint, final String key, boolean def) {
        LinearLayout r = optionRow(parent, name, hint);
        final TextView sw = label("", 11, TEXT, true, 0.15f);
        sw.setGravity(Gravity.CENTER);
        sw.setMinWidth(dp(54));
        sw.setPadding(dp(6), dp(5), dp(6), dp(5));
        final Runnable paint = () -> {
            boolean on = prefs.getBoolean(key, def);
            sw.setText(on ? "ON" : "OFF");
            sw.setTextColor(on ? TEXT : TEXT_DIM);
            sw.setBackground(on ? bevel(RED, RED_DARK, 0xffff6a5e, 0xffff4a3e, RED) : bevel(STEEL_LIGHT, STEEL, STEEL_EDGE, 0xff5a6066, 0xff33373b));
        };
        paint.run();
        sw.setOnClickListener(v -> {
            prefs.edit().putBoolean(key, !prefs.getBoolean(key, def)).apply();
            paint.run();
        });
        r.addView(sw);
    }

    // ---------------------------------------------------------------- game folder and languages
    void refreshGame() {
        if (!hasStorage()) {
            gameText.setText("STORAGE ACCESS NEEDED  \u2022  tap PLAY to grant it");
            playButton.setEnabled(true); playButton.setAlpha(1f);
            playButton.setOnClickListener(v -> requestStorage());
            gameText.setTextColor(YELLOW);
            languages = new ArrayList<>(Collections.singletonList(prefs.getString("language", "russian")));
            showLanguage();
            return;
        }
        // a remembered game file that is not this app's build any more: look in its folder again
        if (!gamePath.isEmpty() && new File(gamePath).exists()) {
            File p = new File(gamePath).getParentFile();
            File g = p == null ? null : gameFileIn(p);
            String np = g == null ? "" : g.getAbsolutePath();
            if (!np.equals(gamePath)) { gamePath = np; prefs.edit().putString("gamepath", gamePath).apply(); }
        }
        if ((gamePath.isEmpty() || !new File(gamePath).exists()) && !busy) {
            // search in the background, the stripe bar runs meanwhile
            busy = true;
            backdrop.setIndeterminate(true);
            status("SEARCHING FOR THE GAME...", YELLOW);
            new Thread(() -> {
                final String found = findGame();
                runOnUiThread(() -> {
                    busy = false;
                    backdrop.setIndeterminate(false);
                    backdrop.setProgress(0f);
                    if (found != null) { gamePath = found; prefs.edit().putString("gamepath", gamePath).apply(); status("", YELLOW); }
                    else { gamePath = ""; status("", YELLOW); }
                    showGame();
                });
            }).start();
            return;
        }
        showGame();
    }

    void showGame() {
        playButton.setOnClickListener(v -> play());
        if (gamePath.isEmpty()) {
            gameText.setText((BuildConfig.STEAM ? "STEAM VERSION (ra3_1.13.game) NOT FOUND" : "GAME (ra3_1.12.game) NOT FOUND") + "  \u2022  choose GAME FOLDER");
            gameText.setTextColor(YELLOW);
            setPlayEnabled(false);
        } else {
            gameText.setText("GAME: " + gameRoot());
            gameText.setTextColor(TEXT_DIM);
        }
        scanLanguages();
    }

    String gameRoot() {
        File dir = new File(gamePath).getParentFile();
        if (dir != null && dir.getName().equalsIgnoreCase("data") && dir.getParentFile() != null) dir = dir.getParentFile();
        return dir == null ? "" : dir.getAbsolutePath();
    }

    void scanLanguages() {
        languages.clear();
        File[] files = gamePath.isEmpty() ? null : new File(gameRoot()).listFiles();
        if (files != null)
            for (File f : files) {
                String n = f.getName().toLowerCase(Locale.ROOT);
                // RA3_<language>_<version>.SkuDef: the language config of the game build in use (1.12, Steam 1.13)
                String suffix = "_" + gameVersion() + ".skudef";
                if (n.startsWith("ra3_") && n.endsWith(suffix)) {
                    String l = n.substring(4, n.length() - suffix.length());
                    if (!l.isEmpty() && !languages.contains(l)) languages.add(l);
                }
            }
        Collections.sort(languages);
        if (languages.isEmpty()) languages.add(prefs.getString("language", "russian"));
        String cur = prefs.getString("language", "");
        if (!languages.contains(cur)) prefs.edit().putString("language", languages.contains("english") ? "english" : languages.get(0)).apply();
        showLanguage();
        scanMods();
        checkFiles();
    }

    // ---------------------------------------------------------------- mods
    static File childCI(File dir, String name) {
        File[] l = dir == null ? null : dir.listFiles();
        if (l != null) for (File f : l) if (f.getName().equalsIgnoreCase(name)) return f;
        return null;
    }

    // a mod folder can hold several versions (Mod_1.0.skudef, Mod_1.1.skudef): the newest one
    static int cmpVersion(String a, String b) {
        String[] x = a.split("[^0-9]+"), y = b.split("[^0-9]+");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            long p = i < x.length && !x[i].isEmpty() ? Long.parseLong(x[i].length() > 12 ? x[i].substring(0, 12) : x[i]) : -1;
            long q = i < y.length && !y[i].isEmpty() ? Long.parseLong(y[i].length() > 12 ? y[i].substring(0, 12) : y[i]) : -1;
            if (p != q) return Long.compare(p, q);
        }
        return a.compareToIgnoreCase(b);
    }

    void scanMods() {
        mods.clear();
        mods.add("");
        File md = gamePath.isEmpty() ? null : childCI(new File(gameRoot()), "Mods");
        File[] dirs = md == null ? null : md.listFiles();
        if (dirs != null) {
            List<String> found = new ArrayList<>();
            for (File d : dirs) {
                if (!d.isDirectory()) continue;
                File[] fs = d.listFiles();
                String best = null;
                if (fs != null)
                    for (File f : fs)
                        if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".skudef") && (best == null || cmpVersion(f.getName(), best) > 0)) best = f.getName();
                if (best != null) found.add(d.getName() + "/" + best);
            }
            Collections.sort(found, String.CASE_INSENSITIVE_ORDER);
            mods.addAll(found);
        }
        if (!mods.contains(prefs.getString("mod", ""))) prefs.edit().putString("mod", "").apply();
        showMod();
    }

    void stepMod(int d) {
        int i = mods.indexOf(prefs.getString("mod", ""));
        i = ((i < 0 ? 0 : i) + d + mods.size()) % mods.size();
        prefs.edit().putString("mod", mods.get(i)).apply();
        showMod();
    }

    void showMod() {
        if (modValue == null) return;
        String m = prefs.getString("mod", "");
        if (m.isEmpty()) { modValue.setText(mods.size() > 1 ? "NONE" : "NONE FOUND"); return; }
        String f = m.substring(m.indexOf('/') + 1);
        modValue.setText(f.substring(0, f.length() - ".skudef".length()).replace('_', ' ').toUpperCase(Locale.ROOT));
    }

    // the log is on by default in test builds (nightly) and off in releases (it costs CPU time and storage)
    static boolean logDefault() { return !Character.isDigit(BuildConfig.VERSION_NAME.isEmpty() ? 'x' : BuildConfig.VERSION_NAME.charAt(0)); }

    static final int[] UI_SCALES = {100, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200};
    TextView uiValue;

    void stepUiScale(int d) {
        int cur = prefs.getInt("ui_scale", 110), i = 0;
        for (int k = 0; k < UI_SCALES.length; k++) if (UI_SCALES[k] <= cur) i = k;
        i = Math.max(0, Math.min(UI_SCALES.length - 1, i + d));
        prefs.edit().putInt("ui_scale", UI_SCALES[i]).apply();
        showUiScale();
    }

    void showUiScale() { uiValue.setText(prefs.getInt("ui_scale", 110) + "%"); }

    void stepLanguage(int d) {
        if (languages.isEmpty()) return;
        int i = languages.indexOf(prefs.getString("language", languages.get(0)));
        i = ((i < 0 ? 0 : i) + d + languages.size()) % languages.size();
        prefs.edit().putString("language", languages.get(i)).apply();
        showLanguage();
        if (!gamePath.isEmpty()) checkFiles();
    }

    void showLanguage() {
        String l = prefs.getString("language", languages.isEmpty() ? "russian" : languages.get(0));
        langValue.setText(l.toUpperCase(Locale.ROOT).replace('_', ' '));
        warnText.setText(languages.size() <= 1 ? "Only one language is installed in this copy of the game." : "");
    }

    // internal storage first, then memory cards and USB drives
    List<File> storageRoots() {
        List<File> roots = new ArrayList<>();
        List<String> seen = new ArrayList<>();
        List<File> cand = new ArrayList<>();
        cand.add(Environment.getExternalStorageDirectory());
        try {
            StorageManager sm = (StorageManager) getSystemService(Context.STORAGE_SERVICE);
            for (StorageVolume v : sm.getStorageVolumes()) {
                File d = null;
                if (Build.VERSION.SDK_INT >= 30) d = v.getDirectory();
                else try { d = (File) StorageVolume.class.getMethod("getPathFile").invoke(v); } catch (Exception ignored) {}
                if (d != null) cand.add(d);
            }
        } catch (Exception ignored) {}
        File[] st = new File("/storage").listFiles();
        if (st != null) for (File f : st) if (!f.getName().equals("emulated") && !f.getName().equals("self")) cand.add(f);
        for (File f : cand) {
            String key;
            try { key = f.getCanonicalPath(); } catch (Exception e) { key = f.getAbsolutePath(); }
            if (seen.contains(key) || f.listFiles() == null) continue;
            seen.add(key);
            roots.add(f);
        }
        return roots;
    }

    String rootLabel(File root) {
        if (root.equals(Environment.getExternalStorageDirectory())) return "Internal storage";
        try {
            StorageManager sm = (StorageManager) getSystemService(Context.STORAGE_SERVICE);
            StorageVolume v = sm.getStorageVolume(root);
            if (v != null) return v.getDescription(this);
        } catch (Exception ignored) {}
        return root.getName();
    }

    // The game build this app runs: RA3 Native - the retail / EA app 1.12, RA3 Native Steam - the Steam 1.13 (the
    // Steam folder also holds the old 1.12, which the Steam app ignores). File name, SHA-256 (runtime/game_build.h).
    static final String[] GAME_FILES = BuildConfig.STEAM ? new String[] {"ra3_1.13.game"} : new String[] {"ra3_1.12.game"};
    static final String[] GAME_SHA256 = BuildConfig.STEAM
        ? new String[] {"e4016847423869b3d4e6a5ae4c74228f2083c65e34416104635602c5526b3eee"}    // 1.13, Steam
        : new String[] {"5d6540ca9d2ee431a99d8051f5cfe22f8240b42a3b52d0de094dda8d7d4508e8"};   // 1.12, retail / EA app
    static boolean isGameFile(String name) { for (String g : GAME_FILES) if (name.equalsIgnoreCase(g)) return true; return false; }
    // the preferred game file directly in this folder
    static File gameFileIn(File dir) {
        File[] l = dir.listFiles();
        if (l == null) return null;
        for (String g : GAME_FILES) for (File f : l) if (f.getName().equalsIgnoreCase(g)) return f;
        return null;
    }
    String gameVersion() { return BuildConfig.STEAM ? "1.13" : "1.12"; }
    boolean steamBuild() { return BuildConfig.STEAM; }
    Class<?> gameActivity() { return steamBuild() ? MainActivity113.class : MainActivity.class; }

    // the game file up to three levels below the root of the internal storage, a memory card or a USB drive
    String findGame() {
        ArrayDeque<File> q = new ArrayDeque<>();
        ArrayDeque<Integer> depth = new ArrayDeque<>();
        for (File r : storageRoots()) { q.add(r); depth.add(0); }
        int visited = 0;
        while (!q.isEmpty() && visited++ < 20000) {
            File d = q.poll(); int dd = depth.poll();
            File[] list = d.listFiles();
            if (list == null) continue;
            for (File f : list) {
                if (isGameFile(f.getName())) { File g = gameFileIn(d); return (g != null ? g : f).getAbsolutePath(); }
                if (f.isDirectory() && dd < 3 && !f.getName().startsWith(".") && !f.getName().equalsIgnoreCase("android")) { q.add(f); depth.add(dd + 1); }
            }
        }
        return null;
    }

    void chooseFolder() {
        if (!hasStorage()) { requestStorage(); return; }
        browse(gamePath.isEmpty() ? Environment.getExternalStorageDirectory() : new File(gameRoot()));
    }

    void browse(final File dir) {
        File[] list = dir.listFiles();
        final List<File> dirs = new ArrayList<>();
        if (list != null) for (File f : list) if (f.isDirectory() && !f.getName().startsWith(".")) dirs.add(f);
        Collections.sort(dirs, (a, c) -> a.getName().compareToIgnoreCase(c.getName()));
        // at the top of a storage the other storages (memory card, USB drive) are offered instead of ".."
        final List<File> roots = storageRoots();
        boolean isRoot = false;
        for (File r : roots) if (r.getAbsolutePath().equals(dir.getAbsolutePath())) isRoot = true;
        final List<File> others = new ArrayList<>();
        if (isRoot || dir.getParentFile() == null) for (File r : roots) if (!r.getAbsolutePath().equals(dir.getAbsolutePath())) others.add(r);
        final boolean canUp = !isRoot && dir.getParentFile() != null;
        List<String> names = new ArrayList<>();
        if (canUp) names.add("\u2191  ..");
        for (File r : others) names.add("\u21C4  " + rootLabel(r));
        for (File f : dirs) names.add("\u25A0  " + f.getName());
        final File game = findIn(dir);
        AlertDialog.Builder bld = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(isRoot ? rootLabel(dir) : dir.getAbsolutePath())
                .setItems(names.toArray(new String[0]), (dlg, which) -> {
                    int i = which;
                    if (canUp) { if (i == 0) { browse(dir.getParentFile()); return; } i--; }
                    if (i < others.size()) { browse(others.get(i)); return; }
                    browse(dirs.get(i - others.size()));
                })
                .setNegativeButton("CANCEL", null);
        if (game != null) bld.setPositiveButton("USE THIS FOLDER", (dlg, w) -> {
            gamePath = game.getAbsolutePath();
            prefs.edit().putString("gamepath", gamePath).apply();
            refreshGame();
        });
        bld.show();
    }

    // the game file in this folder or in its Data subfolder
    File findIn(File dir) {
        for (File base : new File[]{dir, new File(dir, "Data"), new File(dir, "data")}) {
            File g = gameFileIn(base);
            if (g != null) return g;
        }
        return null;
    }

    void requestStorage() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    // ---------------------------------------------------------------- launch
    void setPlayEnabled(boolean on) {
        playButton.setEnabled(on);
        playButton.setAlpha(on ? 1f : 0.35f);
    }

    void play() {
        if (!hasStorage()) { requestStorage(); return; }
        if (busy || !filesOk) return;
        status("STARTING...", YELLOW);
        launch();
    }

    boolean filesOk;
    int checkGen;
    // runs in the background when the launcher opens (and after the language or folder changes): PLAY unlocks when done
    void checkFiles() {
        filesOk = false;
        setPlayEnabled(false);
        if (gamePath.isEmpty()) { status("GAME NOT FOUND - CHOOSE THE GAME FOLDER", RED_GLOW); return; }
        final int gen = ++checkGen;
        busy = true;
        backdrop.setProgress(0f);
        final String lang = prefs.getString("language", "russian");
        final String ver = gameVersion();
        final File gameFile = new File(gamePath);
        new Thread(() -> {
            // the files the language configuration asks for (RA3_<lang>_<version>.SkuDef and the configs it chains to)
            File root = new File(gameRoot());
            List<String> files = new ArrayList<>();
            ArrayDeque<String> configs = new ArrayDeque<>();
            configs.add("RA3_" + lang + "_" + ver + ".SkuDef");
            List<String> seen = new ArrayList<>();
            while (!configs.isEmpty()) {
                String c = configs.poll();
                if (seen.contains(c.toLowerCase(Locale.ROOT))) continue;
                seen.add(c.toLowerCase(Locale.ROOT));
                File cf = resolve(root, c);
                files.add(c);
                if (cf == null) continue;
                for (String line : readText(cf).split("\n")) {
                    String[] w = line.trim().split("\\s+", 2);
                    if (w.length < 2) continue;
                    if (w[0].equalsIgnoreCase("add-big")) files.add(w[1].trim());
                    else if (w[0].equalsIgnoreCase("add-config")) configs.add(w[1].trim());
                }
            }
            files.add("Data/" + gameFile.getName());
            List<String> missing = new ArrayList<>();
            long start = System.currentTimeMillis();
            for (int i = 0; i < files.size(); i++) {
                String f = files.get(i);
                File h = resolve(root, f);
                if (h == null || !h.canRead() || h.length() == 0) missing.add(f.replace('\\', '/'));
                else if (f.toLowerCase(Locale.ROOT).endsWith(".big")) preloadIndex(h);
                if (gen != checkGen) return;   // superseded by a newer check
                status("LOADING GAME FILES  " + (i + 1) + " / " + files.size(), YELLOW);
                final float pr = (i + 1) / (float) files.size();
                runOnUiThread(() -> backdrop.setProgress(pr));
                // keep the bar readable even when the check is instant
                long due = start + (long) (1200 * pr);
                long now = System.currentTimeMillis();
                if (due > now) try { Thread.sleep(due - now); } catch (InterruptedException ignored) {}
            }
            // the exact build: the recompiled code only runs the file it was made from
            final String sha = sha256(gameFile);
            boolean known = false;
            for (String h : GAME_SHA256) if (h.equals(sha)) known = true;
            final boolean buildOk = known;
            final boolean libOk = hasLibrary(steamBuild() ? "ra3native113" : "ra3native");
            runOnUiThread(() -> {
                if (gen != checkGen) return;
                busy = false;
                if (!libOk) { status("THIS APK HAS NO GAME LIBRARY - REINSTALL IT", RED_GLOW); return; }
                if (!buildOk) {
                    status("UNSUPPORTED GAME BUILD: " + gameFile.getName() + "  SHA-256 " + (sha.length() > 12 ? sha.substring(0, 12) : sha) + "...  -  SEE THE README", RED_GLOW);
                    return;
                }
                // the game skips archives that are not there (e.g. MapsExtra.big is absent from many copies):
                // only the executable and the language configuration itself are required
                boolean critical = false;
                for (String m : missing) { String l = m.toLowerCase(Locale.ROOT); if (l.endsWith(".game") || l.endsWith(".skudef")) critical = true; }
                if (!critical) { filesOk = true; setPlayEnabled(true); status("READY", 0xff7ddc6a); }
                else status("MISSING: " + missing.get(0) + (missing.size() > 1 ? "  (+" + (missing.size() - 1) + " MORE)" : "") + "  -  CHECK THE GAME FOLDER", RED_GLOW);
            });
        }).start();
    }

    // the native library is in the APK (installs keep libraries inside the APK, uncompressed, instead of extracting them)
    boolean hasLibrary(String name) {
        String file = "lib" + name + ".so";
        if (new File(getApplicationInfo().nativeLibraryDir, file).exists()) return true;
        try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(getApplicationInfo().sourceDir)) {
            return z.getEntry("lib/arm64-v8a/" + file) != null;
        } catch (Exception e) { return false; }
    }

    static String sha256(File f) {
        try (java.io.FileInputStream in = new java.io.FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            for (int k; (k = in.read(buf)) > 0; ) md.update(buf, 0, k);
            StringBuilder sb = new StringBuilder();
            for (byte x : md.digest()) sb.append(String.format("%02x", x & 255));
            return sb.toString();
        } catch (Exception e) { return ""; }
    }

    // read the archive's header and file index, so it is already in memory when the game opens the archive
    static void preloadIndex(File f) {
        try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
            byte[] h = new byte[16];
            r.readFully(h);
            int index = ((h[12] & 255) << 24) | ((h[13] & 255) << 16) | ((h[14] & 255) << 8) | (h[15] & 255);   // big-endian
            int n = (int) Math.min(Math.max(index, 0), Math.min(r.length(), 8L << 20));
            byte[] buf = new byte[64 << 10];
            for (int done = 16; done < n; ) { int k = r.read(buf, 0, Math.min(buf.length, n - done)); if (k <= 0) break; done += k; }
        } catch (Exception ignored) {}
    }

    // a path from the game's configs (backslashes, any letter case) inside the game folder
    static File resolve(File root, String rel) {
        File cur = root;
        for (String part : rel.replace('\\', '/').split("/")) {
            if (part.isEmpty()) continue;
            File exact = new File(cur, part);
            if (exact.exists()) { cur = exact; continue; }
            File[] l = cur.listFiles();
            File hit = null;
            if (l != null) for (File f : l) if (f.getName().equalsIgnoreCase(part)) { hit = f; break; }
            if (hit == null) return null;
            cur = hit;
        }
        return cur;
    }

    void launch() {
        StringBuilder cfg = new StringBuilder();
        cfg.append("language=").append(prefs.getString("language", "russian")).append('\n');
        cfg.append("cutscenes=").append(prefs.getBoolean("cutscenes", true) ? 1 : 0).append('\n');
        if (!prefs.getString("mod", "").isEmpty()) cfg.append("mod=").append(prefs.getString("mod", "")).append('\n');
        cfg.append("log=").append(prefs.getBoolean("log", logDefault()) ? 1 : 0).append('\n');
        cfg.append("ui_scale=").append(prefs.getInt("ui_scale", 110)).append('\n');
        cfg.append("wide_screen=").append(prefs.getBoolean("wide_screen", false) ? 1 : 0).append('\n');
        cfg.append("fast_shaders=").append(prefs.getBoolean("fast_shaders", false) ? 1 : 0).append('\n');
        cfg.append("button_size=").append(prefs.getInt("button_size", 100)).append('\n');
        cfg.append("button_opacity=").append(prefs.getInt("button_opacity", 100)).append('\n');
        cfg.append("rotate_speed=").append(prefs.getInt("rotate_speed", 300)).append('\n');
        cfg.append("pan_invert=").append(prefs.getBoolean("pan_invert", false) ? 1 : 0).append('\n');
        if (!gamePath.isEmpty()) cfg.append("gamepath=").append(gamePath).append('\n');
        try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "launcher.cfg"))) {
            o.write(cfg.toString().getBytes("UTF-8"));
        } catch (Exception ignored) {}
        startActivity(new Intent(this, gameActivity()));
        finish();
    }

    static String readText(File f) {
        StringBuilder sb = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            String l;
            while ((l = r.readLine()) != null) sb.append(l).append('\n');
        } catch (Exception ignored) {}
        return sb.toString();
    }

    // ---------------------------------------------------------------- drawing
    // dark battlefield backdrop: red glow, scan lines, hazard stripes along the bottom edge
    class Backdrop extends View {
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        float target = 0f, shown = 0f;
        boolean indeterminate;
        Backdrop(Context c) { super(c); }
        void setProgress(float v) { target = v; if (v < shown) shown = v; invalidate(); }
        void setIndeterminate(boolean on) { indeterminate = on; invalidate(); }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            p.setShader(new LinearGradient(0, 0, 0, h, 0xff0c0d0f, 0xff050505, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, p);
            p.setShader(new RadialGradient(w * 0.28f, h * 0.35f, Math.max(w, h) * 0.6f, 0x66a00d10, 0x00000000, Shader.TileMode.CLAMP));
            c.drawRect(0, 0, w, h, p);
            p.setShader(null);
            p.setColor(0x0cffffff);
            for (int y = 0; y < h; y += dp(3)) c.drawRect(0, y, w, y + 1, p);
            // red bar + hazard stripes at the bottom
            int bh = dp(8);
            p.setColor(RED_DARK);
            c.drawRect(0, h - bh - dp(3), w, h - bh, p);
            p.setColor(0xff111111);
            c.drawRect(0, h - bh, w, h, p);
            // hazard stripes double as the progress bar: dim track, bright filled part (or a moving block while searching)
            long t = System.currentTimeMillis();
            float off = indeterminate ? (t / 12f) % (bh * 2) : 0f;
            float fillFrom = 0, fillTo = w * Math.max(0f, Math.min(1f, shown));
            if (indeterminate) { float span = w * 0.25f, pos = (t / 4f) % (w + span); fillFrom = pos - span; fillTo = pos; }
            Path s = new Path();
            for (float x = -bh * 2 + off; x < w + bh; x += bh * 2) {
                s.reset();
                s.moveTo(x, h); s.lineTo(x + bh, h - bh); s.lineTo(x + bh * 2, h - bh); s.lineTo(x + bh, h); s.close();
                boolean lit = x + bh >= fillFrom && x <= fillTo;
                p.setColor(lit ? YELLOW : 0x40ffc21a);
                c.drawPath(s, p);
            }
            if (!indeterminate && shown < target) { shown = Math.min(target, shown + 0.04f); postInvalidateOnAnimation(); }
            if (indeterminate) postInvalidateOnAnimation();
            // thin top rule
            p.setColor(0x55d0161a);
            c.drawRect(0, dp(6), w, dp(7), p);
        }
    }

    // steel panel with bevelled edge and a red corner notch
    class PanelDrawable extends android.graphics.drawable.Drawable {
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        @Override
        public void draw(Canvas c) {
            android.graphics.Rect b = getBounds();
            float cut = dp(18);
            Path path = new Path();
            path.moveTo(b.left + cut, b.top); path.lineTo(b.right, b.top); path.lineTo(b.right, b.bottom - cut);
            path.lineTo(b.right - cut, b.bottom); path.lineTo(b.left, b.bottom); path.lineTo(b.left, b.top + cut); path.close();
            p.setStyle(Paint.Style.FILL);
            p.setShader(new LinearGradient(0, b.top, 0, b.bottom, 0xe0202327, 0xe0121416, Shader.TileMode.CLAMP));
            c.drawPath(path, p);
            p.setShader(null);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(dp(1.5f));
            p.setColor(STEEL_EDGE);
            c.drawPath(path, p);
            p.setStyle(Paint.Style.FILL);
            p.setColor(RED);
            Path notch = new Path();
            notch.moveTo(b.left, b.top + cut); notch.lineTo(b.left + cut, b.top); notch.lineTo(b.left + cut + dp(6), b.top);
            notch.lineTo(b.left, b.top + cut + dp(6)); notch.close();
            c.drawPath(notch, p);
        }

        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    // slanted menu button; brighter while pressed
    class ButtonDrawable extends android.graphics.drawable.Drawable {
        final boolean primary;
        boolean pressed;
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        ButtonDrawable(boolean primary) { this.primary = primary; }

        @Override public boolean isStateful() { return true; }

        @Override
        protected boolean onStateChange(int[] state) {
            boolean pr = false;
            for (int s : state) if (s == android.R.attr.state_pressed) pr = true;
            if (pr != pressed) { pressed = pr; invalidateSelf(); return true; }
            return false;
        }

        @Override
        public void draw(Canvas c) {
            android.graphics.Rect b = getBounds();
            float sl = dp(14);
            Path path = new Path();
            path.moveTo(b.left, b.top); path.lineTo(b.right - sl, b.top); path.lineTo(b.right, b.top + (b.height() / 2f));
            path.lineTo(b.right - sl, b.bottom); path.lineTo(b.left, b.bottom); path.close();
            int top, bot, edge;
            if (primary) { top = pressed ? 0xffff4a3e : RED; bot = pressed ? RED : RED_DARK; edge = 0xffff7a6e; }
            else { top = pressed ? 0xff5e646a : STEEL_LIGHT; bot = pressed ? STEEL_LIGHT : STEEL; edge = pressed ? RED_GLOW : STEEL_EDGE; }
            p.setStyle(Paint.Style.FILL);
            p.setShader(new LinearGradient(0, b.top, 0, b.bottom, top, bot, Shader.TileMode.CLAMP));
            c.drawPath(path, p);
            p.setShader(null);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(dp(1.5f));
            p.setColor(edge);
            c.drawPath(path, p);
            // accent bar on the left
            p.setStyle(Paint.Style.FILL);
            p.setColor(primary ? YELLOW : RED);
            c.drawRect(b.left, b.top, b.left + dp(5), b.bottom, p);
        }

        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    // "V0.2.0 · BUILD 1A2B3C4D" for releases, "NIGHTLY · BUILD 1A2B3C4D" for test builds
    private static String versionLabel() {
        String id = BuildConfig.BUILD_ID.substring(0, Math.min(8, BuildConfig.BUILD_ID.length())).toUpperCase(Locale.ROOT);
        String v = BuildConfig.VERSION_NAME;
        String head = Character.isDigit(v.isEmpty() ? 'x' : v.charAt(0)) ? "V" + v : v.toUpperCase(Locale.ROOT);
        return head + " \u00b7 BUILD " + id;
    }
}
