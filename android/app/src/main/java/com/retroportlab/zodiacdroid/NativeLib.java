package com.retroportlab.zodiacdroid;

import android.view.Surface;

/** The emulator core (core/ at the repository root) and its thread, in libzodiacdroid.so. */
final class NativeLib {
    static { System.loadLibrary("zodiacdroid"); }

    static native boolean checkRom(byte[] rom);
    static native boolean boot(byte[] rom, String autosavePath, String cardPath, boolean cardChanged);
    static native boolean cardPut(String cardPath, String pathOnCard, byte[] data);
    static native String cardList(String cardPath, String folder);
    static native String lastError();
    static native void setSurface(Surface s);
    static native void start();
    static native void stop();
    static native void setPaused(boolean p);
    static native void setFilter(int mode);
    static native float getFps();
    static native void setClock(int mhz);      // 0 = auto
    static native int getClock();

    static native void setTouch(int x, int y, boolean down);   // 480x320 screen coordinates
    static native void setStick(int x, int y);                  // -32767..32767
    static native void setButtons(int mask);

    static native boolean saveState(String path);
    static native boolean loadState(String path);
    static native boolean autosave();
    static native void softReset();
    static native void hardReset();
    static native short[] screenshot();

    // Zodiac buttons (core/zodiac.h)
    static final int HOME = 1, FUNCTION = 1 << 1, POWER = 1 << 2,
            A = 1 << 3, B = 1 << 4, C = 1 << 5, D = 1 << 6,
            L = 1 << 7, R = 1 << 8, BOOKMARK = 1 << 9, EXTRA = 1 << 10;

    static final int SCREEN_W = 480, SCREEN_H = 320;
}
