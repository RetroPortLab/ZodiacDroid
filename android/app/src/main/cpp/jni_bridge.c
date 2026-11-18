/* ZodiacDroid JNI bridge: runs the emulator on its own thread at 60 Hz and copies each
 * 480x320 frame to the Surface, optionally upscaled with a scanline or LCD-grid filter.
 * Input from the UI thread is latched here and applied by the emulator thread at the start
 * of each frame, so the core is only ever touched from one thread. */
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "zodiac.h"
#include "fatimg.h"
#include <aaudio/AAudio.h>
#include <fcntl.h>
#include <sys/stat.h>

#define TAG "ZodiacDroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *window;
static int win_w, win_h;
static pthread_t thread;
static volatile int running, paused, filter;
static volatile int clock_setting;      /* 0 = auto, else MHz */
static int cur_mhz = 200;
static volatile float fps;
static char last_error[256];
static char autosave_path[1024];
static int card_fd = -1;
static volatile int reinsert_in;   /* frames until the card goes back in after a forced eject */
static AAudioStream *audio_stream;

/* latched input */
static volatile int touch_x, touch_y, touch_down;
static volatile int stick_x, stick_y;
static volatile uint32_t buttons;

/* requests handled on the emulator thread: 0 none, else an action code; result in req_result */
enum { REQ_NONE, REQ_SAVE, REQ_LOAD, REQ_SOFT_RESET, REQ_HARD_RESET, REQ_AUTOSAVE };
static pthread_mutex_t req_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t req_cond = PTHREAD_COND_INITIALIZER;
static int req, req_done, req_result;
static char req_path[1024];

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/* ---------------------------------------------------------------- state files */

static int write_file(const char *path, const uint8_t *d, size_t n) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(d, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp, path) == 0;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 0 ? malloc((size_t)len) : NULL;
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)len;
    return d;
}

static int save_to(const char *path) {
    uint8_t *d;
    size_t n = zod_save_state(&d);
    if (!n) return 0;
    int ok = write_file(path, d, n);
    free(d);
    return ok;
}

static int load_from(const char *path) {
    size_t n;
    uint8_t *d = read_file(path, &n);
    if (!d) return 0;
    int ok = zod_load_state(d, n);
    free(d);
    return ok;
}

static void handle_request(void) {
    pthread_mutex_lock(&req_lock);
    int r = req;
    char path[1024];
    memcpy(path, req_path, sizeof path);
    pthread_mutex_unlock(&req_lock);
    if (r == REQ_NONE) return;
    int res = 1;
    switch (r) {
    case REQ_SAVE: res = save_to(path); break;
    case REQ_LOAD: res = load_from(path); break;
    case REQ_SOFT_RESET: zod_soft_reset(); break;
    case REQ_HARD_RESET: zod_reset(); remove(autosave_path); break;
    case REQ_AUTOSAVE: res = autosave_path[0] ? save_to(autosave_path) : 0; break;
    }
    LOGI("request %d -> %d", r, res);
    pthread_mutex_lock(&req_lock);
    req = REQ_NONE; req_result = res; req_done = 1;
    pthread_cond_broadcast(&req_cond);
    pthread_mutex_unlock(&req_lock);
}

/* Post a request and wait for the emulator thread (or run it here if the thread is stopped). */
static int request(int r, const char *path) {
    pthread_mutex_lock(&req_lock);
    req = r; req_done = 0;
    snprintf(req_path, sizeof req_path, "%s", path ? path : "");
    if (!running) {
        pthread_mutex_unlock(&req_lock);
        handle_request();
        pthread_mutex_lock(&req_lock);
    } else {
        while (!req_done) pthread_cond_wait(&req_cond, &req_lock);
    }
    int res = req_result;
    pthread_mutex_unlock(&req_lock);
    return res;
}

/* ---------------------------------------------------------------- video */

static inline uint32_t rgbx(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint32_t dim(uint32_t c, int num) {   /* scale RGB by num/256 */
    uint32_t r = (c & 0xFF) * num >> 8, g = ((c >> 8) & 0xFF) * num >> 8, b = ((c >> 16) & 0xFF) * num >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void present(void) {
    pthread_mutex_lock(&win_lock);
    if (window) {
        int scale = filter ? 3 : 1;
        int w = SCREEN_W * scale, h = SCREEN_H * scale;
        if (w != win_w || h != win_h) {
            ANativeWindow_setBuffersGeometry(window, w, h, WINDOW_FORMAT_RGBX_8888);
            win_w = w; win_h = h;
        }
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(window, &b, NULL) == 0) {
            if (b.width >= w && b.height >= h) {
                for (int y = 0; y < SCREEN_H; y++) {
                    const uint16_t *src = &zod.fb[y * SCREEN_W];
                    if (scale == 1) {
                        uint32_t *dst = (uint32_t *)b.bits + (size_t)y * b.stride;
                        for (int x = 0; x < SCREEN_W; x++) dst[x] = rgbx(src[x]);
                        continue;
                    }
                    for (int sy = 0; sy < 3; sy++) {
                        uint32_t *dst = (uint32_t *)b.bits + (size_t)(y * 3 + sy) * b.stride;
                        int row_dim = 256;
                        if (filter == 2 && sy == 2) row_dim = 140;          /* scanlines */
                        if (filter == 3 && sy == 2) row_dim = 190;          /* LCD grid */
                        for (int x = 0; x < SCREEN_W; x++) {
                            uint32_t c = rgbx(src[x]);
                            uint32_t c0 = row_dim == 256 ? c : dim(c, row_dim);
                            uint32_t c2 = filter == 3 ? dim(c, row_dim * 190 >> 8) : c0;
                            dst[x * 3] = c0; dst[x * 3 + 1] = c0; dst[x * 3 + 2] = c2;
                        }
                    }
                }
            }
            ANativeWindow_unlockAndPost(window);
        }
    }
    pthread_mutex_unlock(&win_lock);
}

/* ---------------------------------------------------------------- emulator thread */

static void apply_input(void) {
    static int last_down, last_x, last_y;
    int d = touch_down, x = touch_x, y = touch_y;
    if (d != last_down || (d && (x != last_x || y != last_y))) {
        zod_touch(x, y, d);
        last_down = d; last_x = x; last_y = y;
    }
    zod_stick(stick_x, stick_y);
    zod_buttons(buttons);
}

static void *run(void *arg) {
    (void)arg;
    const int64_t frame_ns = 1000000000LL / 60;
    int64_t next = now_ns(), fps_t0 = next;
    int fps_frames = 0;
    while (running) {
        handle_request();
        if (paused) {
            usleep(10000);
            next = fps_t0 = now_ns(); fps_frames = 0;
            continue;
        }
        apply_input();
        if (reinsert_in > 0 && --reinsert_in == 0 && card_fd >= 0) sd_attach(card_fd, (uint64_t)lseek(card_fd, 0, SEEK_END));
        zod_run_frame();
        present();

        fps_frames++;
        int64_t t = now_ns();
        /* Auto clock: when the host can't keep up, slow the emulated CPU so game time stays real
         * time (games then draw fewer frames); speed back up when there's room again. */
        {
            static int behind, ahead;
            int want = clock_setting;
            if (!want) {
                int64_t lag = t - (next + frame_ns);
                if (lag > frame_ns / 2) { ahead = 0; if (++behind >= 30 && cur_mhz > 80) { cur_mhz = cur_mhz * 9 / 10; behind = 0; } }
                else if (lag < -frame_ns / 3) { behind = 0; if (++ahead >= 120 && cur_mhz < 200) { cur_mhz += 10; if (cur_mhz > 200) cur_mhz = 200; ahead = 0; } }
                else { behind = ahead = 0; }
                want = cur_mhz;
            }
            if (want != (int)(zod.cpu_hz / 1000000u)) zod_set_cpu_mhz(want);
        }
        if (t - fps_t0 >= 1000000000LL) { fps = fps_frames * 1e9f / (float)(t - fps_t0); fps_t0 = t; fps_frames = 0; }
        next += frame_ns;
        if (next > t) {
            struct timespec ts = { (time_t)(next / 1000000000LL), (long)(next % 1000000000LL) };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        } else if (t - next > frame_ns * 4) {
            next = t;                                      /* running slow: don't try to catch up */
        }
    }
    return NULL;
}

static void stop_thread(void) {
    if (!running) return;
    running = 0;
    pthread_join(thread, NULL);
}

/* ---------------------------------------------------------------- audio */

static aaudio_data_callback_result_t audio_cb(AAudioStream *st, void *user, void *data, int32_t frames) {
    (void)st; (void)user;
    int16_t *out = data;
    int got = paused ? 0 : zod_audio_read(out, frames);
    if (got < frames) memset(out + got * 2, 0, (size_t)(frames - got) * 4);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_open(void) {
    if (audio_stream) return;
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, (int32_t)audio_rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &audio_stream) == AAUDIO_OK) AAudioStream_requestStart(audio_stream);
    else audio_stream = NULL;
    AAudioStreamBuilder_delete(b);
}

static void audio_close(void) {
    if (!audio_stream) return;
    AAudioStream_requestStop(audio_stream);
    AAudioStream_close(audio_stream);
    audio_stream = NULL;
}

/* ---------------------------------------------------------------- SD card image */

#define CARD_MB 512

static int card_open_rw(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size == 0 && !fat_format(fd, (uint64_t)CARD_MB << 20)) { close(fd); return -1; }
    return fd;
}

/* ---------------------------------------------------------------- JNI */

#define FN(name) Java_com_retroportlab_zodiacdroid_NativeLib_##name

static void jstr(JNIEnv *env, jstring s, char *out, size_t n) {
    out[0] = 0;
    if (!s) return;
    const char *p = (*env)->GetStringUTFChars(env, s, NULL);
    snprintf(out, n, "%s", p);
    (*env)->ReleaseStringUTFChars(env, s, p);
}

JNIEXPORT jboolean JNICALL FN(checkRom)(JNIEnv *env, jclass c, jbyteArray rom) {
    (void)c;
    stop_thread();
    last_error[0] = 0;
    jsize len = (*env)->GetArrayLength(env, rom);
    jbyte *b = (*env)->GetByteArrayElements(env, rom, NULL);
    int ok = zod_load_rom((const uint8_t *)b, (size_t)len, last_error, sizeof last_error);
    (*env)->ReleaseByteArrayElements(env, rom, b, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* Load the ROM and power on: resume from the autosave (the handheld's memory) when there is one. */
JNIEXPORT jboolean JNICALL FN(boot)(JNIEnv *env, jclass c, jbyteArray rom, jstring autosave, jstring card, jboolean card_changed) {
    if (!FN(checkRom)(env, c, rom)) return JNI_FALSE;
    jstr(env, autosave, autosave_path, sizeof autosave_path);
    char cp[1024]; jstr(env, card, cp, sizeof cp);
    if (card_fd >= 0) { close(card_fd); card_fd = -1; }
    if (cp[0]) card_fd = card_open_rw(cp);
    zod.model = 2;
    zod_reset();
    if (autosave_path[0] && access(autosave_path, F_OK) == 0) {
        if (!load_from(autosave_path)) {
            LOGI("autosave unreadable; starting fresh");
            zod_reset();
        }
    }
    /* Files were added while the handheld was off: show it the card being taken out and put back,
     * so Palm OS drops what it had cached about the old contents. */
    if (card_fd >= 0 && card_changed) { sd_attach(-1, 0); reinsert_in = 90; }
    else if (card_fd >= 0) sd_attach(card_fd, (uint64_t)lseek(card_fd, 0, SEEK_END));
    else sd_attach(-1, 0);
    touch_down = 0; buttons = 0; stick_x = stick_y = 0;
    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL FN(lastError)(JNIEnv *env, jclass c) { (void)c; return (*env)->NewStringUTF(env, last_error); }

JNIEXPORT void JNICALL FN(setSurface)(JNIEnv *env, jclass c, jobject surface) {
    (void)c;
    pthread_mutex_lock(&win_lock);
    if (window) { ANativeWindow_release(window); window = NULL; }
    win_w = win_h = 0;
    if (surface) window = ANativeWindow_fromSurface(env, surface);
    pthread_mutex_unlock(&win_lock);
    if (window && paused) present();   /* keep showing the last frame behind menus */
}

JNIEXPORT void JNICALL FN(start)(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    audio_open();
    if (running) return;
    paused = 0;
    running = 1;
    pthread_create(&thread, NULL, run, NULL);
}

JNIEXPORT void JNICALL FN(stop)(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    stop_thread();
    audio_close();
    sd_attach(-1, 0);
    if (card_fd >= 0) { fsync(card_fd); close(card_fd); card_fd = -1; }
}

/* Copy a file onto the SD card image (the emulator must be stopped). */
JNIEXPORT jboolean JNICALL FN(cardPut)(JNIEnv *env, jclass c, jstring card, jstring path, jbyteArray data) {
    (void)c;
    char cp[1024], pp[1024];
    jstr(env, card, cp, sizeof cp); jstr(env, path, pp, sizeof pp);
    int fd = card_open_rw(cp);
    if (fd < 0) return JNI_FALSE;
    jsize len = (*env)->GetArrayLength(env, data);
    jbyte *b = (*env)->GetByteArrayElements(env, data, NULL);
    int ok = fat_write_file(fd, pp, (const uint8_t *)b, (uint32_t)len);
    (*env)->ReleaseByteArrayElements(env, data, b, JNI_ABORT);
    fsync(fd); close(fd);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* "name\tsize\tisdir" lines for a folder of the card image ("" = root) */
JNIEXPORT jstring JNICALL FN(cardList)(JNIEnv *env, jclass c, jstring card, jstring path) {
    (void)c;
    char cp[1024], pp[1024];
    jstr(env, card, cp, sizeof cp); jstr(env, path, pp, sizeof pp);
    static char out[65536];
    out[0] = 0;
    int fd = open(cp, O_RDONLY);
    if (fd >= 0) { int n = fat_list(fd, pp, out, sizeof out); out[n < 0 ? 0 : n] = 0; close(fd); }
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT void JNICALL FN(setPaused)(JNIEnv *env, jclass c, jboolean p) { (void)env; (void)c; paused = p; }
JNIEXPORT void JNICALL FN(setFilter)(JNIEnv *env, jclass c, jint f) { (void)env; (void)c; filter = f; }
JNIEXPORT jfloat JNICALL FN(getFps)(JNIEnv *env, jclass c) { (void)env; (void)c; return fps; }
JNIEXPORT void JNICALL FN(setClock)(JNIEnv *env, jclass c, jint mhz) { (void)env; (void)c; clock_setting = mhz; if (!mhz) cur_mhz = 200; }
JNIEXPORT jint JNICALL FN(getClock)(JNIEnv *env, jclass c) { (void)env; (void)c; return (jint)(zod.cpu_hz / 1000000u); }

JNIEXPORT void JNICALL FN(setTouch)(JNIEnv *env, jclass c, jint x, jint y, jboolean down) {
    (void)env; (void)c;
    touch_x = x; touch_y = y; touch_down = down;
}
JNIEXPORT void JNICALL FN(setStick)(JNIEnv *env, jclass c, jint x, jint y) { (void)env; (void)c; stick_x = x; stick_y = y; }
JNIEXPORT void JNICALL FN(setButtons)(JNIEnv *env, jclass c, jint mask) { (void)env; (void)c; buttons = (uint32_t)mask; }

JNIEXPORT jboolean JNICALL FN(saveState)(JNIEnv *env, jclass c, jstring path) {
    (void)c; char p[1024]; jstr(env, path, p, sizeof p);
    return request(REQ_SAVE, p) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL FN(loadState)(JNIEnv *env, jclass c, jstring path) {
    (void)c; char p[1024]; jstr(env, path, p, sizeof p);
    return request(REQ_LOAD, p) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL FN(autosave)(JNIEnv *env, jclass c) { (void)env; (void)c; return request(REQ_AUTOSAVE, NULL) ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT void JNICALL FN(softReset)(JNIEnv *env, jclass c) { (void)env; (void)c; request(REQ_SOFT_RESET, NULL); }
JNIEXPORT void JNICALL FN(hardReset)(JNIEnv *env, jclass c) { (void)env; (void)c; request(REQ_HARD_RESET, NULL); }

/* RGB565 copy of the current screen, for save-slot thumbnails */
JNIEXPORT jshortArray JNICALL FN(screenshot)(JNIEnv *env, jclass c) {
    (void)c;
    jshortArray a = (*env)->NewShortArray(env, SCREEN_W * SCREEN_H);
    if (a) (*env)->SetShortArrayRegion(env, a, 0, SCREEN_W * SCREEN_H, (const jshort *)zod.fb);
    return a;
}
