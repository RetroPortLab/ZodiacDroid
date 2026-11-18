package com.retroportlab.zodiacdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.text.format.DateUtils;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.text.DateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class MainActivity extends Activity {
    // ---------------------------------------------------------------- look --
    private static final int BG = 0xFF0A1022, PANEL = 0xFF142042, PANEL_FOCUS = 0xFF213468;
    private static final int ACCENT = 0xFF5FB4FF, GOLD = 0xFFFFC247, TEXT = 0xFFF2F4FA, SUBTEXT = 0xFF97A3C2;

    private static final int PICK_ROM = 1, PICK_INSTALL = 2;
    private static final int SCREEN_HOME = 0, SCREEN_SETTINGS = 1, SCREEN_GAME = 2, SCREEN_UPDATE = 3,
            SCREEN_STATES = 4, SCREEN_CONTROLS = 5;
    private static final int SLOTS = 6;
    private static final String[] FILTERS = {"Smooth", "Sharp pixels", "Scanlines", "LCD grid"};

    private SharedPreferences prefs;
    private FrameLayout root;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private int screen = SCREEN_HOME;

    private Updater.Release foundUpdate = null;
    private volatile boolean updateBusy = false, updateCancel = false;

    // in-game state
    private boolean running = false, menuOpen = false;
    private int keyMask = 0, axisButtons = 0;
    private View pauseMenu = null;
    private TextView fpsView;
    private android.os.Vibrator vibrator;
    private boolean vibrating = false;
    /* Follows the Zodiac's vibration motor: on while the OS has it switched on. */
    private final Runnable vibTick = new Runnable() {
        @Override public void run() {
            if (screen != SCREEN_GAME) { stopVibe(); return; }
            boolean want = prefs.getBoolean("vibration", true) && !menuOpen && NativeLib.getVibrate();
            if (want != vibrating) {
                if (vibrator == null) vibrator = (android.os.Vibrator) getSystemService(VIBRATOR_SERVICE);
                if (vibrator != null && vibrator.hasVibrator()) {
                    if (want) vibrator.vibrate(android.os.VibrationEffect.createOneShot(5000, android.os.VibrationEffect.DEFAULT_AMPLITUDE));
                    else vibrator.cancel();
                }
                vibrating = want;
            }
            ui.postDelayed(this, 30);
        }
    };
    private void stopVibe() {
        ui.removeCallbacks(vibTick);
        if (vibrating && vibrator != null) vibrator.cancel();
        vibrating = false;
    }
    private final Runnable fpsTick = new Runnable() {
        @Override public void run() {
            if (fpsView == null || screen != SCREEN_GAME) return;
            fpsView.setText(String.format(Locale.US, "%.0f FPS  %d MHz  %s", NativeLib.getFps(), NativeLib.getClock(),
                    prefs.getBoolean("jit", true) && NativeLib.jitAvailable() ? "JIT" : "INT"));
            ui.postDelayed(this, 500);
        }
    };

    // ------------------------------------------------------------ lifecycle --
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = getSharedPreferences("zodiacdroid", MODE_PRIVATE);
        root = new FrameLayout(this);
        root.setBackgroundColor(BG);
        setContentView(root);
        hideSystemUi();
        showHome();
    }

    @Override protected void onPause() {
        super.onPause();
        if (running) {
            NativeLib.setPaused(true);
            NativeLib.autosave();       // the handheld's memory: installed apps, settings, progress
        }
    }

    @Override protected void onResume() {
        super.onResume();
        hideSystemUi();
        if (screen == SCREEN_GAME && !menuOpen) NativeLib.setPaused(false);
    }

    @Override protected void onDestroy() {
        if (running) { NativeLib.setPaused(true); NativeLib.autosave(); NativeLib.stop(); }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    // ------------------------------------------------------------- widgets --
    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private GradientDrawable rounded(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(10));
        if (stroke != 0) g.setStroke(dp(2), stroke);
        return g;
    }

    private StateListDrawable cardBg() {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[]{android.R.attr.state_focused}, rounded(PANEL_FOCUS, ACCENT));
        s.addState(new int[]{android.R.attr.state_pressed}, rounded(PANEL_FOCUS, ACCENT));
        s.addState(new int[]{}, rounded(PANEL, 0));
        return s;
    }

    private TextView text(String s, float sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private LinearLayout row(String title, String subtitle, String value, View.OnClickListener click) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(dp(16), dp(10), dp(16), dp(10));
        r.setBackground(cardBg());
        r.setFocusable(true);
        r.setClickable(true);
        r.setOnClickListener(click);
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        TextView t = text(title, 17, TEXT, true);
        t.setTag("title");
        texts.addView(t);
        if (subtitle != null && !subtitle.isEmpty()) {
            TextView s = text(subtitle, 13, SUBTEXT, false);
            s.setTag("subtitle");
            texts.addView(s);
        }
        r.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
        if (value != null) {
            TextView v = text(value, 15, ACCENT, true);
            v.setTag("value");
            r.addView(v);
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = dp(6);
        r.setLayoutParams(lp);
        return r;
    }

    private void setRowValue(View rowView, String value) {
        View v = rowView.findViewWithTag("value");
        if (v instanceof TextView) ((TextView) v).setText(value);
    }

    private void setRowTitle(View rowView, String value) {
        View v = rowView.findViewWithTag("title");
        if (v instanceof TextView) ((TextView) v).setText(value);
    }

    private TextView sectionTitle(String s) {
        TextView t = text(s.toUpperCase(Locale.US), 12, SUBTEXT, true);
        t.setLetterSpacing(0.12f);
        t.setPadding(dp(4), dp(14), 0, dp(6));
        return t;
    }

    private LinearLayout header(String subtitle, View... actions) {
        LinearLayout h = new LinearLayout(this);
        h.setOrientation(LinearLayout.HORIZONTAL);
        h.setGravity(Gravity.CENTER_VERTICAL);
        h.setPadding(dp(24), dp(16), dp(24), dp(8));
        TextView logo = text("ZODIAC", 26, ACCENT, true);
        logo.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        TextView logo2 = text("DROID", 26, TEXT, true);
        logo2.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD_ITALIC));
        h.addView(logo);
        h.addView(logo2);
        h.addView(text("   " + subtitle, 14, SUBTEXT, false), new LinearLayout.LayoutParams(0, -2, 1));
        for (View a : actions) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = dp(8);
            h.addView(a, lp);
        }
        return h;
    }

    private TextView pill(String label, View.OnClickListener click) {
        TextView b = text(label, 14, TEXT, true);
        b.setPadding(dp(16), dp(8), dp(16), dp(8));
        b.setBackground(cardBg());
        b.setFocusable(true);
        b.setClickable(true);
        b.setOnClickListener(click);
        return b;
    }

    private void page(String subtitle, LinearLayout list, View... actions) {
        root.removeAllViews();
        root.setBackgroundColor(BG);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.addView(header(subtitle, actions));
        list.setOrientation(LinearLayout.VERTICAL);
        ScrollView sv = new ScrollView(this);
        sv.setFillViewport(true);
        sv.setPadding(dp(24), 0, dp(24), dp(16));
        sv.setClipToPadding(false);
        sv.addView(list);
        page.addView(sv, new LinearLayout.LayoutParams(-1, 0, 1));
        TextView foot = text("A  select     B  back     Select + Start  in-game menu", 12, SUBTEXT, false);
        foot.setPadding(dp(28), dp(4), 0, dp(10));
        page.addView(foot);
        root.addView(page, new FrameLayout.LayoutParams(-1, -1));
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (Exception e) { return "?"; }
    }

    private File romFile() { return new File(getFilesDir(), "zodiac.rom"); }
    private boolean haveRom() { return romFile().length() > 0; }
    private File autosaveFile() { return new File(getFilesDir(), "memory.zst"); }
    private File cardFile() { return new File(getFilesDir(), "sdcard.img"); }
    private File slotFile(int n) { return new File(getFilesDir(), "slot" + n + ".zst"); }
    private File slotThumb(int n) { return new File(getFilesDir(), "slot" + n + ".png"); }

    // ---------------------------------------------------------------- home --
    private void showHome() {
        stopEmu();
        screen = SCREEN_HOME; menuOpen = false; pauseMenu = null;
        ui.removeCallbacks(fpsTick);
        LinearLayout list = new LinearLayout(this);
        page("v" + versionName(), list, pill("Settings", v -> showSettings()));

        if (!haveRom()) {
            list.addView(sectionTitle("Welcome"));
            TextView intro = text("ZodiacDroid runs the Tapwave Zodiac's own Palm OS, so it needs the ROM from your Zodiac. "
                    + "Choose either the raw ROM dump or Tapwave's ROM update (zodiac_1.1_update.prc, or the .zip it came in).",
                    15, SUBTEXT, false);
            intro.setPadding(dp(4), 0, dp(4), dp(12));
            list.addView(intro);
            View choose = row("Choose Zodiac ROM", "Copied into the app once", null, v -> pick(PICK_ROM));
            list.addView(choose);
            choose.requestFocus();
            return;
        }

        if (foundUpdate != null) list.addView(updateBanner());
        else autoCheckForUpdate(list);

        list.addView(sectionTitle("Zodiac"));
        boolean resume = autosaveFile().exists();
        View power = row(resume ? "Power on" : "Power on (first start)",
                resume ? "Carries on where you left off: your apps, settings and progress are kept"
                        : "Palm OS will walk you through setup: tap the screen, calibrate the stick with the left stick and Start",
                "▶", v -> powerOn(null));
        list.addView(power);
        list.addView(row("Save states", "Snapshots of the whole handheld you can go back to", null, v -> showStates(false)));

        list.addView(sectionTitle("SD card"));
        list.addView(row("Install games and apps", "Tapwave store .zip files, or .prc / .pdb files; they go on the emulated SD card",
                null, v -> pickInstall()));
        list.addView(row("What's on the card", cardSummary(), null, v -> showCard()));

        list.addView(sectionTitle("Controls"));
        TextView help = text("Left stick or D-pad: analog controller   ·   A: select   B: back   X Y: other action buttons   ·   L1 R1: triggers\n"
                + "Start: Function   ·   Select: Home   ·   Touch the screen to use the stylus\n"
                + "Select + Start (or Back): pause menu", 13, SUBTEXT, false);
        help.setPadding(dp(4), 0, dp(4), dp(8));
        help.setLineSpacing(0, 1.2f);
        list.addView(help);
        power.requestFocus();
    }

    // ------------------------------------------------------------ settings --
    private void showSettings() {
        screen = SCREEN_SETTINGS;
        LinearLayout list = new LinearLayout(this);
        page("Settings", list, pill("Back", v -> showHome()));
        list.addView(sectionTitle("Handheld"));
        View rom = row("ROM", "Raw dump or zodiac_1.1_update.prc", haveRom() ? "Loaded" : "Not set", v -> pick(PICK_ROM));
        list.addView(rom);
        list.addView(row("Controls", "Choose which button does what", null, v -> showControls()));
        list.addView(row("Erase the handheld's memory", "Like a hard reset: removes the apps and data you added (save states are kept)",
                null, v -> confirm("Erase the Zodiac's memory?",
                        "Installed apps, settings and game progress inside the emulated Zodiac are removed. Your save states are kept.",
                        "Erase", () -> {
                            if (!autosaveFile().delete() && autosaveFile().exists()) toast("Couldn't erase");
                            else toast("Memory erased; Palm OS setup runs on the next power on");
                        })));

        list.addView(sectionTitle("Display"));
        LinearLayout filter = row("Screen filter", "How the 480×320 picture is scaled up", FILTERS[filterMode()], null);
        filter.setOnClickListener(v -> {
            int f = (filterMode() + 1) % FILTERS.length;
            prefs.edit().putInt("filter", f).apply();
            setRowValue(filter, FILTERS[f]);
        });
        list.addView(filter);
        LinearLayout vib = row("Vibration", "Rumble when the game or Palm OS vibrates",
                prefs.getBoolean("vibration", true) ? "On" : "Off", null);
        vib.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("vibration", true);
            prefs.edit().putBoolean("vibration", on).apply();
            setRowValue(vib, on ? "On" : "Off");
        });
        list.addView(vib);
        LinearLayout assist = row("Stick tap assist", "Holds quick stick taps long enough for menus to see them. Can make the stick overshoot in games",
                prefs.getBoolean("stickAssist", false) ? "On" : "Off", null);
        assist.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("stickAssist", false);
            prefs.edit().putBoolean("stickAssist", on).apply();
            NativeLib.setStickAssist(on);
            setRowValue(assist, on ? "On" : "Off");
        });
        list.addView(assist);
        LinearLayout stretch = row("Fill the screen", "Stretch to the full display instead of keeping 3:2",
                prefs.getBoolean("stretch", false) ? "On" : "Off", null);
        stretch.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("stretch", false);
            prefs.edit().putBoolean("stretch", on).apply();
            setRowValue(stretch, on ? "On" : "Off");
        });
        list.addView(stretch);
        LinearLayout fps = row("Show frame rate", null, prefs.getBoolean("showFps", false) ? "On" : "Off", null);
        fps.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("showFps", false);
            prefs.edit().putBoolean("showFps", on).apply();
            setRowValue(fps, on ? "On" : "Off");
        });
        list.addView(fps);

        list.addView(sectionTitle("Performance"));
        LinearLayout jit = row("CPU emulation", "The recompiler translates the Zodiac's ARM code for this device's CPU "
                + "and is several times faster. Switch to the interpreter if a game misbehaves",
                prefs.getBoolean("jit", true) ? "Recompiler" : "Interpreter", null);
        jit.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("jit", true);
            prefs.edit().putBoolean("jit", on).apply();
            setRowValue(jit, on ? "Recompiler" : "Interpreter");
        });
        list.addView(jit);
        LinearLayout clock = row("CPU clock", "The Zodiac runs at 200 MHz. Higher clocks make the game's menus and "
                + "loading snappier if your device can keep up; lower clocks can stall 3D games", clockName(clockSetting()), null);
        clock.setOnClickListener(v -> {
            int i = 0;
            for (int k = 0; k < CLOCKS.length; k++) if (CLOCKS[k] == clockSetting()) i = k;
            int c = CLOCKS[(i + 1) % CLOCKS.length];
            prefs.edit().putInt("clock2", c).apply();
            NativeLib.setClock(c);
            setRowValue(clock, clockName(c));
        });
        list.addView(clock);

        list.addView(sectionTitle("Updates"));
        LinearLayout auto = row("Check automatically", "Looks on GitHub for a new version at most twice a day",
                prefs.getBoolean("autoUpdate", true) ? "On" : "Off", null);
        auto.setOnClickListener(v -> {
            boolean on = !prefs.getBoolean("autoUpdate", true);
            prefs.edit().putBoolean("autoUpdate", on).apply();
            setRowValue(auto, on ? "On" : "Off");
        });
        list.addView(auto);
        final View[] check = new View[1];
        check[0] = row("Check for updates now", "You have v" + versionName(), "", v -> checkForUpdate(check[0]));
        list.addView(check[0]);

        list.addView(sectionTitle("About"));
        TextView about = text("ZodiacDroid v" + versionName() + " is an experimental Tapwave Zodiac emulator written from scratch "
                + "(ARM920T, i.MX1 and ATI W4200). It needs your own Zodiac ROM. "
                + "Not affiliated with Tapwave or PalmSource.", 14, SUBTEXT, false);
        about.setPadding(dp(4), 0, dp(4), dp(12));
        list.addView(about);
        rom.requestFocus();
    }

    private static final int[] CLOCKS = {200, 300, 400, 166, 133};
    private static String clockName(int mhz) { return mhz == 0 ? "Auto" : mhz + " MHz"; }
    private int clockSetting() { int c = prefs.getInt("clock2", 200); return c == 0 ? 200 : c; }

    private int filterMode() { return Math.max(0, Math.min(FILTERS.length - 1, prefs.getInt("filter", 2))); }

    private void confirm(String title, String msg, String yes, Runnable action) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(msg)
                .setPositiveButton(yes, (d, w) -> action.run())
                .setNegativeButton("Cancel", null).show();
    }

    // ------------------------------------------------------------ controls --
    /** Zodiac buttons the player can assign, in display order. */
    private static final int[] ZB = {NativeLib.A, NativeLib.B, NativeLib.C, NativeLib.D, NativeLib.L, NativeLib.R,
            NativeLib.FUNCTION, NativeLib.HOME, NativeLib.BOOKMARK, NativeLib.POWER};
    private static final String[] ZB_NAMES = {"Action A", "Action B", "Action C", "Action D", "Left trigger", "Right trigger",
            "Function", "Home", "Bookmark", "Power"};
    private static final int[] ZB_DEFAULT_KEYS = {KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_L2, 0};

    private int keyFor(int i) { return prefs.getInt("key_" + i, ZB_DEFAULT_KEYS[i]); }

    private int mapKey(int code) {
        int m = 0;
        for (int i = 0; i < ZB.length; i++) if (code != 0 && keyFor(i) == code) m |= ZB[i];
        return m;
    }

    private int capturing = -1;
    private boolean trigL, trigR;   // analog trigger axes currently pressed (for capture)

    private void captureKey(int code) {
        int k = code == KeyEvent.KEYCODE_BACK ? 0 : code;
        prefs.edit().putInt("key_" + capturing, k).apply();
        setRowValue(capturingRow, keyName(k));
        capturing = -1;
    }

    private View capturingRow = null;

    private static String keyName(int code) {
        if (code == 0) return "None";
        String s = KeyEvent.keyCodeToString(code).replace("KEYCODE_", "").replace("BUTTON_", "").replace('_', ' ');
        return s.length() > 1 ? s.charAt(0) + s.substring(1).toLowerCase(Locale.US) : s;
    }

    private void showControls() {
        screen = SCREEN_CONTROLS;
        capturing = -1;
        LinearLayout list = new LinearLayout(this);
        page("Controls", list, pill("Back", v -> showSettings()), pill("Defaults", v -> {
            SharedPreferences.Editor e = prefs.edit();
            for (int i = 0; i < ZB.length; i++) e.remove("key_" + i);
            e.apply();
            showControls();
        }));
        list.addView(sectionTitle("Buttons"));
        View first = null;
        for (int i = 0; i < ZB.length; i++) {
            final int idx = i;
            LinearLayout r = row(ZB_NAMES[i], null, keyName(keyFor(i)), null);
            r.setOnClickListener(v -> {
                capturing = idx; capturingRow = r;
                setRowValue(r, "Press a button…");
            });
            list.addView(r);
            if (first == null) first = r;
        }
        list.addView(sectionTitle("Fixed"));
        TextView fixed = text("Left stick and D-pad: analog controller.   Touch screen: stylus.\n"
                + "Select + Start or Back: pause menu.   Selecting a row and pressing Back clears it.", 13, SUBTEXT, false);
        fixed.setPadding(dp(4), 0, dp(4), dp(8));
        list.addView(fixed);
        if (first != null) first.requestFocus();
    }

    // -------------------------------------------------------- save states --
    private String slotLabel(int n) {
        File f = slotFile(n);
        if (!f.exists()) return "Empty";
        return DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(new Date(f.lastModified()));
    }

    /** inGame: called from the pause menu (save or load); otherwise from the home screen (load and power on). */
    private void showStates(boolean inGame) {
        screen = SCREEN_STATES;
        LinearLayout list = new LinearLayout(this);
        if (inGame) {
            root.removeView(pauseMenu);
            FrameLayout dim = new FrameLayout(this);
            dim.setBackgroundColor(0xD0000000);
            dim.setClickable(true);
            LinearLayout panel = new LinearLayout(this);
            panel.setOrientation(LinearLayout.VERTICAL);
            panel.setPadding(dp(20), dp(16), dp(20), dp(16));
            panel.setBackground(rounded(BG, 0x40FFFFFF));
            panel.addView(text("Save states", 20, TEXT, true));
            ScrollView sv = new ScrollView(this);
            sv.addView(list);
            panel.addView(sv, new LinearLayout.LayoutParams(-1, -2));
            dim.addView(panel, new FrameLayout.LayoutParams(dp(560), -2, Gravity.CENTER));
            pauseMenu = dim;
            root.addView(dim, new FrameLayout.LayoutParams(-1, -1));
        } else {
            page("Save states", list, pill("Back", v -> showHome()));
        }
        list.setOrientation(LinearLayout.VERTICAL);
        View first = null;
        for (int n = 1; n <= SLOTS; n++) {
            final int slot = n;
            LinearLayout r = row("Slot " + n, slotLabel(n), null, null);
            if (slotThumb(n).exists()) {
                Bitmap b = BitmapFactory.decodeFile(slotThumb(n).getAbsolutePath());
                if (b != null) {
                    ImageView iv = new ImageView(this);
                    iv.setImageBitmap(b);
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(96), dp(64));
                    lp.rightMargin = dp(12);
                    r.addView(iv, 0, lp);
                }
            }
            r.setOnClickListener(v -> {
                if (inGame) slotMenu(slot);
                else if (slotFile(slot).exists()) powerOn(slotFile(slot));
                else toast("That slot is empty");
            });
            list.addView(r);
            if (first == null) first = r;
        }
        if (first != null) first.requestFocus();
    }

    private void slotMenu(int slot) {
        boolean has = slotFile(slot).exists();
        String[] items = has ? new String[]{"Save here (replace)", "Load this state"} : new String[]{"Save here"};
        new AlertDialog.Builder(this).setTitle("Slot " + slot).setItems(items, (d, which) -> {
            if (which == 0) {
                if (NativeLib.saveState(slotFile(slot).getAbsolutePath())) { saveThumb(slot); toast("Saved to slot " + slot); }
                else toast("Couldn't save");
                closePauseMenu();
            } else {
                if (NativeLib.loadState(slotFile(slot).getAbsolutePath())) toast("Loaded slot " + slot);
                else toast("Couldn't load that state");
                closePauseMenu();
            }
        }).show();
    }

    private void saveThumb(int slot) {
        short[] px = NativeLib.screenshot();
        if (px == null) return;
        int w = NativeLib.SCREEN_W, h = NativeLib.SCREEN_H;
        int[] argb = new int[(w / 2) * (h / 2)];
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w / 2; x++) {
                int p = px[(y * 2) * w + x * 2] & 0xFFFF;
                int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                argb[y * (w / 2) + x] = 0xFF000000 | (r << 19) | (g << 10) | (b << 3);
            }
        Bitmap bm = Bitmap.createBitmap(argb, w / 2, h / 2, Bitmap.Config.ARGB_8888);
        try (FileOutputStream o = new FileOutputStream(slotThumb(slot))) { bm.compress(Bitmap.CompressFormat.PNG, 90, o); }
        catch (Exception ignored) { }
    }

    // ------------------------------------------------------------- updates --
    private void autoCheckForUpdate(LinearLayout list) {
        if (!prefs.getBoolean("autoUpdate", true) || updateBusy) return;
        long last = prefs.getLong("updateChecked", 0);
        if (System.currentTimeMillis() - last < 12 * DateUtils.HOUR_IN_MILLIS) return;
        prefs.edit().putLong("updateChecked", System.currentTimeMillis()).apply();
        new Thread(() -> {
            try {
                Updater.Release r = Updater.latest();
                if (r != null && Updater.newer(r.tag, versionName())) ui.post(() -> {
                    foundUpdate = r;
                    if (screen == SCREEN_HOME && list.isAttachedToWindow()) list.addView(updateBanner(), 0);
                });
            } catch (Exception ignored) { }
        }).start();
    }

    private View updateBanner() {
        LinearLayout r = row("Update available: " + foundUpdate.tag, "You have v" + versionName() + ". Select to see what's new and install.",
                "Update", v -> showUpdate(foundUpdate));
        TextView b = (TextView) r.findViewWithTag("value");
        b.setTextColor(BG);
        b.setPadding(dp(10), dp(4), dp(10), dp(4));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(GOLD); bg.setCornerRadius(dp(6));
        b.setBackground(bg);
        return r;
    }

    private void checkForUpdate(View rowView) {
        if (updateBusy) return;
        updateBusy = true;
        setRowValue(rowView, "Checking…");
        new Thread(() -> {
            String msg; Updater.Release found = null;
            try {
                Updater.Release r = Updater.latest();
                if (r == null) msg = "No releases yet";
                else if (Updater.newer(r.tag, versionName())) { found = r; msg = r.tag + " available"; }
                else msg = "Up to date";
            } catch (Exception e) {
                msg = e instanceof java.net.UnknownHostException ? "No internet" : "Couldn't check";
                final String detail = e.getMessage();
                ui.post(() -> { if (detail != null) toast(detail); });
            }
            final String m = msg; final Updater.Release f = found;
            ui.post(() -> {
                updateBusy = false;
                prefs.edit().putLong("updateChecked", System.currentTimeMillis()).apply();
                if (f != null) { foundUpdate = f; showUpdate(f); }
                else if (rowView.isAttachedToWindow()) setRowValue(rowView, m);
            });
        }).start();
    }

    private void showUpdate(Updater.Release r) {
        screen = SCREEN_UPDATE;
        updateCancel = false;
        LinearLayout list = new LinearLayout(this);
        page("Update", list, pill("Back", v -> { updateCancel = true; showSettings(); }));
        list.addView(sectionTitle("ZodiacDroid " + r.tag));
        TextView sub = text("You have v" + versionName() + (r.apkSize > 0
                ? String.format(Locale.US, "  ·  download %.1f MB", r.apkSize / 1048576.0) : ""), 14, SUBTEXT, false);
        sub.setPadding(dp(4), 0, dp(4), dp(10));
        list.addView(sub);
        final View[] go = new View[1];
        go[0] = row("Download and install", "Your ROM, the handheld's memory, save states and settings are kept", null,
                v -> downloadAndInstall(r, go[0]));
        list.addView(go[0]);
        list.addView(row("Open the releases page", Updater.RELEASES_PAGE, null, v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(r.pageUrl))); }
            catch (Exception e) { toast("No web browser found"); }
        }));
        if (r.notes != null && !r.notes.trim().isEmpty()) {
            list.addView(sectionTitle("What's new"));
            TextView notes = text(r.notes.replace("\r", "").replaceAll("(?m)^#+\\s*", "").replace("**", "").trim(), 15, TEXT, false);
            notes.setPadding(dp(4), 0, dp(4), dp(12));
            notes.setLineSpacing(0, 1.15f);
            list.addView(notes);
        }
        go[0].requestFocus();
    }

    private void downloadAndInstall(Updater.Release r, View rowView) {
        if (updateBusy) { updateCancel = true; return; }
        if (!Updater.canInstall(this)) {
            toast("Allow ZodiacDroid to install apps, then press Back and select Download again");
            try { Updater.openInstallPermission(this); } catch (Exception e) { toast("Open Android Settings > Apps > ZodiacDroid > Install unknown apps"); }
            return;
        }
        updateBusy = true; updateCancel = false;
        setRowTitle(rowView, "Downloading…  (select again to cancel)");
        new Thread(() -> {
            String error = null; File apk = null;
            try {
                long[] lastUi = {0};
                apk = Updater.download(this, r, (done, total) -> {
                    long now = System.currentTimeMillis();
                    if (now - lastUi[0] > 200) {
                        lastUi[0] = now;
                        final String t = total > 0 ? String.format(Locale.US, "Downloading…  %d%%  (select again to cancel)", done * 100 / total)
                                : String.format(Locale.US, "Downloading…  %.1f MB", done / 1048576.0);
                        ui.post(() -> setRowTitle(rowView, t));
                    }
                    return !updateCancel;
                });
                if (apk != null) Updater.install(this, apk);
            } catch (Exception e) {
                error = e.getMessage() != null ? e.getMessage() : "Download failed";
            }
            final String err = error; final boolean cancelled = apk == null && err == null;
            ui.post(() -> {
                updateBusy = false;
                if (!rowView.isAttachedToWindow()) return;
                if (err != null) { setRowTitle(rowView, "Download and install"); toast(err); }
                else if (cancelled) setRowTitle(rowView, "Download and install");
                else setRowTitle(rowView, "Installing…  confirm on the next screen");
            });
        }).start();
    }

    // --------------------------------------------------------------- files --
    private void pick(int code) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try { startActivityForResult(i, code); }
        catch (Exception e) { toast("No file picker available"); }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null) return;
        if (req == PICK_INSTALL) {
            java.util.List<Uri> uris = new java.util.ArrayList<>();
            if (data.getClipData() != null) for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
            else if (data.getData() != null) uris.add(data.getData());
            installFiles(uris);
            return;
        }
        if (data.getData() == null) return;
        Uri u = data.getData();
        if (req == PICK_ROM) {
            toast("Reading ROM…");
            new Thread(() -> {
                String err = importRom(u);
                ui.post(() -> {
                    if (err != null) { toast(err); return; }
                    toast("ROM ready");
                    if (screen == SCREEN_SETTINGS) showSettings(); else showHome();
                });
            }).start();
        }
    }

    /** Accepts a raw ROM, Tapwave's update .prc, or a .zip holding either. Returns an error or null. */
    private String importRom(Uri u) {
        byte[] b = readAll(u, 64 << 20);
        if (b == null) return "Couldn't read that file";
        if (b.length > 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) b = romFromZip(b);
        if (b == null) return "No Zodiac ROM found inside that zip";
        if (!NativeLib.checkRom(b)) return NativeLib.lastError();
        try (FileOutputStream o = new FileOutputStream(romFile())) { o.write(b); }
        catch (Exception e) { return "Couldn't save the ROM"; }
        return null;
    }

    private byte[] romFromZip(byte[] zip) {
        try (ZipInputStream z = new ZipInputStream(new java.io.ByteArrayInputStream(zip))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory()) continue;
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[65536];
                for (int n; (n = z.read(buf)) > 0; ) out.write(buf, 0, n);
                byte[] d = out.toByteArray();
                String name = e.getName().toLowerCase(Locale.US);
                if ((name.endsWith(".prc") || name.endsWith(".rom") || name.endsWith(".bin") || d.length >= (4 << 20))
                        && NativeLib.checkRom(d)) return d;
            }
        } catch (Exception ignored) { }
        return null;
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) { }
        return "file";
    }

    private byte[] readAll(Uri uri, int limit) {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] b = new byte[65536];
            for (int n; (n = in.read(b)) > 0; ) {
                out.write(b, 0, n);
                if (out.size() > limit) return null;
            }
            return out.toByteArray();
        } catch (Exception e) { return null; }
    }

    private byte[] readFile(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] b = new byte[(int) f.length()];
            int off = 0;
            for (int n; off < b.length && (n = in.read(b, off, b.length - off)) > 0; ) off += n;
            return off == b.length ? b : null;
        } catch (Exception e) { return null; }
    }

    // ------------------------------------------------------------- SD card --
    private void pickInstall() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        try { startActivityForResult(i, PICK_INSTALL); }
        catch (Exception e) { toast("No file picker available"); }
    }

    private String cardSummary() {
        if (!cardFile().exists()) return "Empty";
        String l = NativeLib.cardList(cardFile().getAbsolutePath(), "PALM/Launcher");
        int apps = l.isEmpty() ? 0 : l.split("\n").length;
        return apps + (apps == 1 ? " app" : " apps") + " in /PALM/Launcher";
    }

    /** One file to put on the card. */
    static final class CardFile { final String path; final byte[] data; CardFile(String p, byte[] d) { path = p; data = d; } }

    private static boolean isPalmDb(String name) {
        String l = name.toLowerCase(Locale.US);
        return l.endsWith(".prc") || l.endsWith(".pdb") || l.endsWith(".pqa");
    }

    private static String baseName(String p) {
        p = p.replace('\\', '/');
        return p.substring(p.lastIndexOf('/') + 1);
    }

    /** What to copy for one picked file: Palm databases go to /PALM/Launcher; a Tapwave store
     *  .zip is unpacked following its installer .ini (RAM files to /PALM/Launcher, CARD files
     *  to the folder named there). */
    private java.util.List<CardFile> plan(String name, byte[] data) throws Exception {
        java.util.List<CardFile> out = new java.util.ArrayList<>();
        boolean zip = data.length > 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 3 && data[3] == 4;
        if (!zip) {
            if (!name.startsWith("._")) out.add(new CardFile(isPalmDb(name) ? "/PALM/Launcher/" + name : "/PALM/Programs/" + name, data));
            return out;
        }
        java.util.Map<String, byte[]> entries = new java.util.LinkedHashMap<>();
        try (ZipInputStream z = new ZipInputStream(new java.io.ByteArrayInputStream(data))) {
            for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                if (e.isDirectory()) continue;
                ByteArrayOutputStream bo = new ByteArrayOutputStream();
                byte[] buf = new byte[65536];
                for (int n; (n = z.read(buf)) > 0; ) bo.write(buf, 0, n);
                entries.put(e.getName().replace('\\', '/'), bo.toByteArray());
            }
        }
        for (java.util.Map.Entry<String, byte[]> e : entries.entrySet()) {
            if (!e.getKey().toLowerCase(Locale.US).endsWith(".ini")) continue;
            String ini = new String(e.getValue(), "ISO-8859-1");
            if (!ini.contains("PalmFile")) continue;
            String dir = e.getKey().contains("/") ? e.getKey().substring(0, e.getKey().lastIndexOf('/') + 1) : "";
            java.util.Map<String, java.util.Map<String, String>> sec = new java.util.HashMap<>();
            java.util.Map<String, String> cur = null;
            for (String line : ini.split("\r?\n")) {
                String t = line.trim();
                if (t.startsWith(";") || t.isEmpty()) continue;
                if (t.startsWith("[") && t.endsWith("]")) { cur = new java.util.HashMap<>(); sec.put(t.substring(1, t.length() - 1).toLowerCase(Locale.US), cur); continue; }
                int eq = t.indexOf('=');
                if (cur != null && eq > 0) cur.put(t.substring(0, eq).trim().toLowerCase(Locale.US), t.substring(eq + 1).trim());
            }
            for (int i = 1; sec.containsKey("palmfile" + i); i++) {
                java.util.Map<String, String> f = sec.get("palmfile" + i);
                String src = (dir + f.getOrDefault("source", "")).replace('\\', '/');
                byte[] d = null;
                for (java.util.Map.Entry<String, byte[]> x : entries.entrySet()) if (x.getKey().equalsIgnoreCase(src)) d = x.getValue();
                if (d == null) throw new Exception("The installer lists " + src + ", which isn't in the zip");
                String file = baseName(src);
                String target = f.getOrDefault("target", "RAM").toUpperCase(Locale.US);
                String folder = f.getOrDefault("folder", "").replace('\\', '/').replaceAll("^/+|/+$", "");
                out.add(new CardFile(target.equals("CARD") && !folder.isEmpty() ? "/" + folder + "/" + file : "/PALM/Launcher/" + file, d));
            }
            return out;
        }
        /* No installer: databases go to /PALM/Launcher, everything else (a game's data folder) goes
         * under /PALM/Programs with its folder layout kept. Mac resource-fork junk is skipped. */
        /* A zip that wraps everything in a folder next to the .prc ("Game/game.prc", "Game/Data/...")
         * is unpacked without that wrapper, so the data lands where the game looks for it. */
        String wrap = "";
        for (String k : entries.keySet()) {
            String kn = k.replaceAll("^/+", "");
            if (isPalmDb(kn) && !kn.startsWith("__MACOSX/") && kn.contains("/")) { wrap = kn.substring(0, kn.indexOf('/') + 1); break; }
        }
        for (java.util.Map.Entry<String, byte[]> e : entries.entrySet()) {
            String n = e.getKey().replaceAll("^/+", "");
            if (!wrap.isEmpty() && n.startsWith(wrap)) n = n.substring(wrap.length());
            String base = baseName(n);
            if (n.startsWith("__MACOSX/") || base.startsWith("._") || base.equals(".DS_Store")) continue;
            if (isPalmDb(n)) out.add(new CardFile("/PALM/Launcher/" + base, e.getValue()));
            else if (!n.toLowerCase(Locale.US).matches(".*\\.(exe|pdf|txt|ini|htm|html|doc)$")) out.add(new CardFile("/PALM/Programs/" + n, e.getValue()));
        }
        return out;
    }

    private void installFiles(java.util.List<Uri> uris) {
        if (uris.isEmpty()) return;
        toast("Installing…");
        new Thread(() -> {
            int copied = 0; String err = null;
            try {
                for (Uri u : uris) {
                    String name = displayName(u);
                    byte[] d = readAll(u, 256 << 20);
                    if (d == null) throw new Exception("Couldn't read " + name);
                    for (CardFile f : plan(name, d)) {
                        if (!NativeLib.cardPut(cardFile().getAbsolutePath(), f.path, f.data)) throw new Exception("The SD card is full or couldn't be written");
                        copied++;
                    }
                }
            } catch (Exception e) { err = e.getMessage(); }
            final int n = copied; final String e2 = err;
            ui.post(() -> {
                if (n > 0) prefs.edit().putBoolean("cardChanged", true).apply();
                if (e2 != null) toast(e2);
                else toast(n + (n == 1 ? " file" : " files") + " copied to the SD card. Find them in the launcher's card category.");
                if (screen == SCREEN_HOME) showHome();
            });
        }).start();
    }

    private void showCard() {
        screen = SCREEN_SETTINGS;
        LinearLayout list = new LinearLayout(this);
        page("SD card", list, pill("Back", v -> showHome()), pill("Install", v -> pickInstall()));
        listCardFolder(list, "", 0);
        if (list.getChildCount() == 0) {
            TextView t = text("The card is empty. Install a Tapwave store .zip or .prc files to fill it.", 15, SUBTEXT, false);
            t.setPadding(dp(4), dp(8), dp(4), dp(8));
            list.addView(t);
        }
        list.addView(sectionTitle("Card"));
        list.addView(row("Uninstall games and apps", "Remove an app or a game's data from the card", null, v -> showUninstall()));
        list.addView(row("Erase the SD card", "Removes everything on the emulated card", null, v -> confirm("Erase the SD card?",
                "Everything on the emulated card is deleted.", "Erase", () -> {
                    if (cardFile().delete()) prefs.edit().putBoolean("cardChanged", true).apply();
                    showCard();
                })));
    }

    /** Lists what is installed (apps in /PALM/Launcher, game data in /PALM/Programs) with a way to remove each. */
    private void showUninstall() {
        screen = SCREEN_SETTINGS;
        LinearLayout list = new LinearLayout(this);
        page("Uninstall", list, pill("Back", v -> showCard()));
        int shown = 0;
        for (String folder : new String[]{"PALM/Launcher", "PALM/Programs"}) {
            if (!cardFile().exists()) break;
            boolean first = true;
            for (String line : NativeLib.cardList(cardFile().getAbsolutePath(), folder).split("\n")) {
                String[] p = line.split("\t");
                if (p.length < 3) continue;
                if (first) { list.addView(sectionTitle(folder.endsWith("Launcher") ? "Apps and games (/PALM/Launcher)" : "Game data (/PALM/Programs)")); first = false; }
                final String path = folder + "/" + p[0];
                boolean dir = p[2].equals("1");
                String sub = dir ? "Folder with everything in it" : String.format(Locale.US, "%.1f MB", Long.parseLong(p[1]) / 1048576.0);
                list.addView(row((dir ? "📁 " : "") + p[0], sub, null, v -> confirm("Remove " + p[0] + "?",
                        dir ? "The folder and everything in it is deleted from the card." : "The file is deleted from the card.", "Remove", () -> {
                            if (NativeLib.cardDelete(cardFile().getAbsolutePath(), path)) prefs.edit().putBoolean("cardChanged", true).apply();
                            else toast("Couldn't remove " + p[0]);
                            showUninstall();
                        })));
                shown++;
            }
        }
        if (shown == 0) {
            TextView t = text("Nothing is installed on the card.", 15, SUBTEXT, false);
            t.setPadding(dp(4), dp(8), dp(4), dp(8));
            list.addView(t);
        } else {
            TextView t = text("A game usually has a file in Apps and a folder in Game data; remove both to uninstall it completely.", 13, SUBTEXT, false);
            t.setPadding(dp(4), dp(12), dp(4), dp(8));
            list.addView(t);
        }
    }

    private void listCardFolder(LinearLayout list, String folder, int depth) {
        if (!cardFile().exists() || depth > 4) return;
        String l = NativeLib.cardList(cardFile().getAbsolutePath(), folder);
        for (String line : l.split("\n")) {
            String[] p = line.split("\t");
            if (p.length < 3) continue;
            boolean dir = p[2].equals("1");
            String indent = new String(new char[depth * 3]).replace('\0', ' ');
            TextView t = text(indent + (dir ? "📁 " : "    ") + p[0] + (dir ? "" : String.format(Locale.US, "   %.1f MB", Long.parseLong(p[1]) / 1048576.0)),
                    14, dir ? TEXT : SUBTEXT, dir);
            t.setPadding(dp(4), dp(2), dp(4), dp(2));
            list.addView(t);
            if (dir) listCardFolder(list, folder.isEmpty() ? p[0] : folder + "/" + p[0], depth + 1);
        }
    }

    // ---------------------------------------------------------------- game --
    private void stopEmu() {
        if (!running) return;
        NativeLib.setPaused(true);
        NativeLib.autosave();
        NativeLib.stop();
        running = false;
    }

    /** Power on the handheld, from its memory (autosave), or from a save state if one is given. */
    private void powerOn(File state) {
        byte[] rom = readFile(romFile());
        if (rom == null) { toast("Choose your Zodiac ROM first"); showSettings(); return; }
        boolean changed = prefs.getBoolean("cardChanged", false);
        if (!NativeLib.boot(rom, autosaveFile().getAbsolutePath(), cardFile().getAbsolutePath(), changed)) { toast(NativeLib.lastError()); return; }
        prefs.edit().putBoolean("cardChanged", false).apply();
        if (state != null && !NativeLib.loadState(state.getAbsolutePath())) toast("Couldn't load that state; started normally");
        running = true;
        enterGame();
    }

    private void enterGame() {
        screen = SCREEN_GAME;
        root.removeAllViews();
        root.setBackgroundColor(Color.BLACK);
        ScreenView view = new ScreenView(this, prefs.getBoolean("stretch", false));
        view.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { NativeLib.setSurface(h.getSurface()); }
            @Override public void surfaceDestroyed(SurfaceHolder h) { NativeLib.setSurface(null); }
        });
        root.addView(view, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        keyMask = axisButtons = 0;
        menuOpen = false; pauseMenu = null;
        fpsView = null;
        ui.removeCallbacks(fpsTick);
        ui.removeCallbacks(vibTick);
        ui.post(vibTick);
        if (prefs.getBoolean("showFps", false)) {
            fpsView = text("-- FPS", 14, 0xFFFFE066, true);
            fpsView.setBackgroundColor(0x80000000);
            fpsView.setPadding(dp(8), dp(2), dp(8), dp(2));
            root.addView(fpsView, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
            ui.postDelayed(fpsTick, 500);
        }
        NativeLib.setStickAssist(prefs.getBoolean("stickAssist", false));
        NativeLib.setFilter(filterMode());
        NativeLib.setClock(clockSetting());
        NativeLib.setJit(prefs.getBoolean("jit", true));
        NativeLib.setButtons(0);
        NativeLib.setStick(0, 0);
        NativeLib.start();
        NativeLib.setPaused(false);
    }

    /** The Zodiac screen: keeps 3:2 (unless stretched) and turns touches into stylus input. */
    static class ScreenView extends SurfaceView {
        private final boolean stretch;
        ScreenView(Context c, boolean stretch) { super(c); this.stretch = stretch; }
        @Override protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            if (!stretch) { if (w * 2 > h * 3) w = h * 3 / 2; else h = w * 2 / 3; }
            setMeasuredDimension(w, h);
        }
        @Override public boolean onTouchEvent(MotionEvent e) {
            int a = e.getActionMasked();
            int x = (int) (e.getX() * NativeLib.SCREEN_W / Math.max(1, getWidth()));
            int y = (int) (e.getY() * NativeLib.SCREEN_H / Math.max(1, getHeight()));
            x = Math.max(0, Math.min(NativeLib.SCREEN_W - 1, x));
            y = Math.max(0, Math.min(NativeLib.SCREEN_H - 1, y));
            boolean down = a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_MOVE || a == MotionEvent.ACTION_POINTER_DOWN;
            if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) down = false;
            NativeLib.setTouch(x, y, down);
            return true;
        }
    }

    private void openPauseMenu() {
        if (menuOpen) return;
        menuOpen = true;
        keyMask = axisButtons = 0;
        NativeLib.setButtons(0);
        NativeLib.setStick(0, 0);
        NativeLib.setTouch(0, 0, false);
        NativeLib.setPaused(true);

        FrameLayout dim = new FrameLayout(this);
        dim.setBackgroundColor(0xB0000000);
        dim.setClickable(true);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(20), dp(20), dp(20));
        panel.setBackground(rounded(BG, 0x40FFFFFF));
        panel.addView(text("Tapwave Zodiac", 20, TEXT, true));
        TextView hint = text("Paused", 13, SUBTEXT, false);
        hint.setPadding(0, 0, 0, dp(12));
        panel.addView(hint);
        View resume = row("Resume", null, null, v -> closePauseMenu());
        panel.addView(resume);
        panel.addView(row("Save states", "Save or load a snapshot", null, v -> showStates(true)));
        LinearLayout filter = row("Screen filter", null, FILTERS[filterMode()], null);
        filter.setOnClickListener(v -> {
            int f = (filterMode() + 1) % FILTERS.length;
            prefs.edit().putInt("filter", f).apply();
            NativeLib.setFilter(f);
            setRowValue(filter, FILTERS[f]);
        });
        panel.addView(filter);
        panel.addView(row("Press Home", "Back to the Zodiac launcher", null, v -> { closePauseMenu(); tapButton(NativeLib.HOME); }));
        panel.addView(row("Remount SD card", "Takes the card out and puts it back, so Palm OS reads it again", null, v -> {
            if (NativeLib.remountCard()) { toast("SD card removed, going back in…"); closePauseMenu(); }
            else toast("There's no SD card to remount");
        }));
        panel.addView(row("Reset", "Like the reset button: restarts Palm OS, keeps your apps and data", null,
                v -> { NativeLib.softReset(); closePauseMenu(); }));
        panel.addView(row("Quit to menu", "The handheld's memory is saved", null, v -> showHome()));
        ScrollView sv = new ScrollView(this);
        sv.addView(panel);
        dim.addView(sv, new FrameLayout.LayoutParams(dp(440), -2, Gravity.CENTER));
        pauseMenu = dim;
        root.addView(dim, new FrameLayout.LayoutParams(-1, -1));
        resume.requestFocus();
    }

    private void closePauseMenu() {
        if (!menuOpen) return;
        if (pauseMenu != null) root.removeView(pauseMenu);
        pauseMenu = null;
        menuOpen = false;
        screen = SCREEN_GAME;
        keyMask = axisButtons = 0;
        NativeLib.setButtons(0);
        NativeLib.setPaused(false);
    }

    /** Press and release a Zodiac button for a quarter of a second. */
    private void tapButton(int b) {
        NativeLib.setButtons(keyMask | axisButtons | b);
        ui.postDelayed(() -> NativeLib.setButtons(keyMask | axisButtons), 250);
    }

    // --------------------------------------------------------------- input --
    private boolean selectHeld, startHeld;
    private int swallowUps = 0;

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN, up = e.getAction() == KeyEvent.ACTION_UP;

        if (screen == SCREEN_CONTROLS && capturing >= 0) {
            if (down && e.getRepeatCount() == 0) captureKey(code);
            return true;
        }
        if (menuOpen && swallowUps > 0 && up && (code == KeyEvent.KEYCODE_BUTTON_START || code == KeyEvent.KEYCODE_BUTTON_SELECT)) {
            swallowUps--;
            return true;
        }
        if (screen != SCREEN_GAME || menuOpen) {
            if (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_DPAD_CENTER) {
                if (up) { View f = getCurrentFocus(); if (f != null) f.performClick(); }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_START) {
                if (up) {
                    if (menuOpen && screen == SCREEN_STATES) { root.removeView(pauseMenu); menuOpen = false; openPauseMenu(); }
                    else if (menuOpen) closePauseMenu();
                    else if (screen == SCREEN_SETTINGS || screen == SCREEN_STATES) showHome();
                    else if (screen == SCREEN_CONTROLS) showSettings();
                    else if (screen == SCREEN_UPDATE) { updateCancel = true; showSettings(); }
                    else if (code == KeyEvent.KEYCODE_BACK) finish();
                }
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_SELECT) return true;
            if (KeyEvent.isGamepadButton(code)) return true;   // never let the system act on shoulder/trigger buttons
            return super.dispatchKeyEvent(e);
        }

        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) selectHeld = down;
        if (code == KeyEvent.KEYCODE_BUTTON_START) startHeld = down;
        if (selectHeld && startHeld) { selectHeld = startHeld = false; swallowUps = 2; openPauseMenu(); return true; }
        if (code == KeyEvent.KEYCODE_BACK) { if (up) openPauseMenu(); return true; }
        if (isDpad(code)) {
            if (down) dpad |= dpadBit(code); else if (up) dpad &= ~dpadBit(code);
            updateStick();
            return true;
        }
        int bit = mapKey(code);
        if (bit == 0) return KeyEvent.isGamepadButton(code) || super.dispatchKeyEvent(e);   // unmapped pad buttons do nothing
        if (down) keyMask |= bit; else if (up) keyMask &= ~bit;
        NativeLib.setButtons(keyMask | axisButtons);
        return true;
    }

    private int dpad = 0;
    private float axisX = 0, axisY = 0;

    private static boolean isDpad(int code) {
        return code == KeyEvent.KEYCODE_DPAD_UP || code == KeyEvent.KEYCODE_DPAD_DOWN
                || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT;
    }

    private static int dpadBit(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP: return 1;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 2;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 4;
            default: return 8;
        }
    }

    /** The analog controller: the left stick, or full deflection from the D-pad. */
    private void updateStick() {
        float x = axisX, y = axisY;
        if ((dpad & 4) != 0) x = -1; else if ((dpad & 8) != 0) x = 1;
        if ((dpad & 1) != 0) y = -1; else if ((dpad & 2) != 0) y = 1;
        if (Math.abs(x) < 0.12f) x = 0;
        if (Math.abs(y) < 0.12f) y = 0;
        NativeLib.setStick(Math.round(x * 32767), Math.round(y * 32767));
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (screen == SCREEN_CONTROLS && capturing >= 0 && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getAction() == MotionEvent.ACTION_MOVE) {
            // L2 / R2 that arrive as analog axes instead of key events
            boolean l = e.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.6f || e.getAxisValue(MotionEvent.AXIS_BRAKE) > 0.6f;
            boolean r = e.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.6f || e.getAxisValue(MotionEvent.AXIS_GAS) > 0.6f;
            if (l && !trigL) captureKey(KeyEvent.KEYCODE_BUTTON_L2);
            else if (r && !trigR) captureKey(KeyEvent.KEYCODE_BUTTON_R2);
            trigL = l; trigR = r;
            return true;
        }
        if (screen == SCREEN_GAME && !menuOpen && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                && e.getAction() == MotionEvent.ACTION_MOVE) {
            axisX = e.getAxisValue(MotionEvent.AXIS_X);
            axisY = e.getAxisValue(MotionEvent.AXIS_Y);
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int d = 0;
            if (hy < -0.5f) d |= 1; if (hy > 0.5f) d |= 2; if (hx < -0.5f) d |= 4; if (hx > 0.5f) d |= 8;
            dpad = (dpad & ~15) | d;
            updateStick();
            int m = 0;
            if (e.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.5f || e.getAxisValue(MotionEvent.AXIS_BRAKE) > 0.5f)
                m |= mapKey(KeyEvent.KEYCODE_BUTTON_L2);
            if (e.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.5f || e.getAxisValue(MotionEvent.AXIS_GAS) > 0.5f)
                m |= mapKey(KeyEvent.KEYCODE_BUTTON_R2);
            axisButtons = m;
            NativeLib.setButtons(keyMask | axisButtons);
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    private void hideSystemUi() {
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    private void toast(String s) { Toast.makeText(this, s, Toast.LENGTH_SHORT).show(); }
}
