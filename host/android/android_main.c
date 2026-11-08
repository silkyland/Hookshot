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

/* the files dir the payload lives in: HOOKSHOT_ANDROID_DATA, else <data>/files/hookshot */
static void data_dir(char *out, size_t cap) {
    const char *e = getenv("HOOKSHOT_ANDROID_DATA");
    if (e && *e) { snprintf(out, cap, "%s", e); return; }
    const char *base = hos_android_app && hos_android_app->activity
                           ? hos_android_app->activity->internalDataPath : NULL;
    if (!base) base = "/data/data/com.hookshot.oot/files";
    snprintf(out, cap, "%s/files/hookshot", base);
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
