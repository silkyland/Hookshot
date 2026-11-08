/* (android) android_main: the game's entry on a NativeActivity (android_native_app_glue's main thread).
   It stores the app so android_input.c's pump can read the AInputQueue, waits for the surface
   (APP_CMD_INIT_WINDOW) unless HOS_HEADLESS=1, then calls the game's main (host/hos_main.c's hos_game_main)
   with an argv that names the app-private files dir: image.bin, romfs/, user/.

   The game files are not in the APK: the app copies (or the sideload flow places) them under
   <files>/hookshot/ — image.bin (~500 MB), romfs/ (~26 GB), plus the built .so's the Vulkan back end needs.
   HOOKSHOT_ANDROID_DATA overrides the directory (default: <nativeLibraryDir>/../files/hookshot, resolved at
   runtime through ANativeActivity's internalDataPath). */
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_activity.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include "../hos.h"
#include "android_input.h"

int hos_game_main(int argc, char **argv);   /* host/hos_main.c */
int hos_display_init(void);                  /* host/android/android_display.c */

struct android_app *hos_android_app;
ANativeWindow *hos_android_window;
int hos_android_quit;

extern volatile int hos_posix_window[4];     /* android_display.c: focused, w, h, fullscreen */

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "hookshot", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "hookshot", __VA_ARGS__)

/* the files dir the payload lives in: HOOKSHOT_ANDROID_DATA, else the first candidate that already
 * holds image.bin — the adb sideload path (app-private), then the no-adb paths a file manager or an
 * HTTP download can reach: the app-specific external dir (no permission needed) and /sdcard/Hookshot
 * (needs "All files access"). With nothing installed yet, the external dir is the writable default. */
static void data_dir(char *out, size_t cap) {
    const char *e = getenv("HOOKSHOT_ANDROID_DATA");
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    static const char *cands[] = {
        NULL,   /* internalDataPath/files/hookshot, filled below */
        "/sdcard/Android/data/dev.hookshot.oot/files/hookshot",
        "/sdcard/Hookshot",
    };
    char internal[4096];
    const char *base = hos_android_app && hos_android_app->activity
                           ? hos_android_app->activity->internalDataPath : NULL;
    /* internalDataPath already ends in .../files */
    if (base) snprintf(internal, sizeof internal, "%s/hookshot", base);
    else snprintf(internal, sizeof internal, "/data/data/dev.hookshot.oot/files/hookshot");
    cands[0] = internal;
    ALOGI("internalDataPath=%s", base ? base : "(null)");
    for (size_t i = 0; i < sizeof cands / sizeof *cands; i++) {
        char img[4096];
        snprintf(img, sizeof img, "%s/image.bin", cands[i]);
        if (access(img, R_OK) == 0) { snprintf(out, cap, "%s", cands[i]); return; }
        ALOGE("cand[%d] %s: %s", (int)i, cands[i], strerror(errno));
    }
    snprintf(out, cap, "%s", cands[1]);
}

static void ensure_dirs(const char *root) {
    char p[4096];
    snprintf(p, sizeof p, "%s/user", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/romfs", root); mkdir(p, 0777);
}

/* lifecycle, on the android_main thread through the glue's looper */
static void on_app_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        hos_android_window = app->window;
        hos_posix_window[0] = 1;
        hos_posix_window[1] = ANativeWindow_getWidth(app->window);
        hos_posix_window[2] = ANativeWindow_getHeight(app->window);
        hos_posix_window[3] = 1;   /* fullscreen is the only mode on Android */
        ALOGI("surface ready %dx%d", hos_posix_window[1], hos_posix_window[2]);
        break;
    case APP_CMD_TERM_WINDOW:
        hos_android_window = NULL;
        hos_posix_window[0] = 0;
        ALOGI("surface gone");
        break;
    case APP_CMD_DESTROY:
        hos_android_quit = 1;
        break;
    }
}

/* ---- the game runs on its own thread (like hos_main.c's Linux path: the window's thread is android_main's) ---- */
typedef struct { char dir[4096]; } BootArg;

static void *game_thread(void *p) {
    BootArg *a = (BootArg *)p;
    ensure_dirs(a->dir);

    /* the game's logs go to stderr — invisible on Android; mirror them into a file we can pull
       (dup2 keeps the fd so logcat stays a fallback) */
    {
        char logp[4096];
        snprintf(logp, sizeof logp, "%s/user/hookshot.log", a->dir);
        int lf = open(logp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
        if (lf >= 0) {
            if (dup2(lf, 2) < 0) close(lf);   /* stderr -> hookshot.log */
            else { setvbuf(stderr, NULL, _IOLBF, 0); }
        }
    }

    char image[4096], romfs[4096], user[4096];
    snprintf(image, sizeof image, "%s/image.bin", a->dir);
    snprintf(romfs, sizeof romfs, "%s/romfs", a->dir);
    snprintf(user, sizeof user, "%s/user", a->dir);
    if (access(image, R_OK) != 0) {
        ALOGE("no image.bin at %s — the game payload is not installed", a->dir);
        hos_android_quit = 1;
        return NULL;
    }

    char *argv[] = {(char *)"hookshot", image, (char *)"--romfs", romfs, (char *)"--user", user, NULL};
    ALOGI("starting: %s", image);
    hos_game_main(6, argv);
    hos_android_quit = 1;
    return NULL;
}

void android_main(struct android_app *app) {
    hos_android_app = app;
    app->userData = NULL;
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = NULL;   /* android_input.c pumps the queue itself */

    BootArg *arg = calloc(1, sizeof *arg);
    data_dir(arg->dir, sizeof arg->dir);
    ALOGI("data dir: %s", arg->dir);
    hos_display_init();   /* hos_posix_window[]'s first read (the surface fills the rest in on_app_cmd) */

    pthread_t game;
    if (pthread_create(&game, NULL, game_thread, arg)) {
        ALOGE("cannot start the game thread");
        return;
    }

    /* android_main's loop: surface lifecycle plus the input pump (the display's loop calls the pump itself,
       but commands must be drained here too — the glue delivers them on this looper) */
    int events;
    struct android_poll_source *source;
    while (!hos_android_quit) {
        while (ALooper_pollOnce(0, NULL, &events, (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) {
                app->destroyRequested = 0;
                hos_android_quit = 1;
            }
        }
        hos_android_pump();   /* input events, drained on the thread that owns the queue */
        usleep(10000);
    }
    pthread_join(game, NULL);
}
