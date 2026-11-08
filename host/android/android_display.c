/* (android) The game's window: what host/posix/display_sdl.c is on Linux, on the NativeActivity's ANativeWindow.
   The window (surface) already exists — android_main.c's android_main keeps it in hos_android_window; the Vulkan
   back end makes its surface through MkwVkWindow (gpu/vk/vk_api.h) with vkCreateAndroidSurfaceKHR.

   The keyboard's state goes to hos_posix_keys (GetAsyncKeyState, for nn_hid.c and the Insert menu), the surface's
   focus and size to hos_posix_window (GetForegroundWindow, GetSystemMetrics). android_input.c fills both from the
   AInputQueue on the android_main thread's loop (hos_display_loop).

   HOS_HEADLESS=1: no surface used (rx_start(NULL): offscreen, as on Linux). */
#include "../hos.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <android/native_window.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include "../../gpu/vk/vk_api.h"

int rx_start(void *hwnd);
void rx_resize(int width, int height);

/* set by android_main.c before the game thread starts (and on every surface change) */
extern ANativeWindow *hos_android_window;

volatile LONG64 hos_present_count;   /* (nvn_native.c: the game's presents) */
__attribute__((visibility("default"))) volatile uint8_t hos_posix_keys[256];
__attribute__((visibility("default"))) volatile int hos_posix_window[4];   /* focused, screen width, screen height, fullscreen */

static volatile LONG mouse_dx, mouse_dy, mouse_captured;
int hos_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
    return mouse_captured;
}
/* (the touch overlay's camera drag feeds the right stick itself; a mouse capture has no meaning on Android) */
void hos_android_mouse_move(int32_t dx, int32_t dy) {
    InterlockedExchangeAdd(&mouse_dx, dx);
    InterlockedExchangeAdd(&mouse_dy, dy);
}
void hos_android_mouse_capture(int on) { mouse_captured = on; }

/* ---- the Vulkan surface: ANativeWindow -> vkCreateAndroidSurfaceKHR ---- */
static int create_surface(void *w, void *instance, void *surface) {
    ANativeWindow *nw = (ANativeWindow *)w;
    if (!nw) return 0;
    VkAndroidSurfaceCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    ci.window = nw;
    PFN_vkCreateAndroidSurfaceKHR fn = (PFN_vkCreateAndroidSurfaceKHR)
        vkGetInstanceProcAddr((VkInstance)instance, "vkCreateAndroidSurfaceKHR");
    if (!fn) return 0;
    return fn((VkInstance)instance, &ci, NULL, (VkSurfaceKHR *)surface) == VK_SUCCESS;
}
static void drawable_size(void *w, int *width, int *height) {
    ANativeWindow *nw = (ANativeWindow *)w;
    *width = nw ? ANativeWindow_getWidth(nw) : 0;
    *height = nw ? ANativeWindow_getHeight(nw) : 0;
}
static MkwVkWindow vk_window = {MKWVK_WINDOW_VERSION, create_surface, drawable_size, NULL};

/* ---- the renderer, as display_sdl.c's renderer_main ---- */
static volatile int renderer;
static HANDLE renderer_up;

static DWORD WINAPI renderer_main(void *p) {
    (void)p;
    vk_window.window = hos_android_window;
    const int ok = rx_start(hos_android_window ? &vk_window : NULL);
    if (!ok) hos_log("display: the renderer did not start: the screen stays black");
    renderer = ok;
    if (ok && hos_android_window) {
        int w = 0, h = 0;
        drawable_size(hos_android_window, &w, &h);
        if (w > 0 && h > 0) rx_resize(w, h);
    }
    SetEvent(renderer_up);
    return 0;
}

void hos_renderer_wait(void) {
    { const char *e = getenv("HOS_NO_RENDERER_WAIT"); if (e && atoi(e) != 0) return; }
    if (renderer_up) WaitForSingleObject(renderer_up, INFINITE);
}

static volatile int start_asked;
static volatile int fullscreen_asked = -1;

/* PostMessageA to the window (host/posix/win32_posix.c): WM_APP + 7, the Insert menu's Fullscreen row. On Android
   fullscreen is the only mode: the row is accepted and ignored (no window border to change). */
__attribute__((visibility("default"))) void hos_posix_post_message(uint32_t msg, uintptr_t wp, intptr_t lp) {
    (void)lp;
    if (msg == WM_APP + 7) __atomic_store_n(&fullscreen_asked, wp ? 1 : 0, __ATOMIC_RELEASE);
}
int hos_display_owns_sdl(void) { return 0; }

/* main()'s halves, kept for android_main.c's shape (hos_main.c's non-Windows main does the same on Linux) */
int hos_display_init(void) {
    if (getenv("HOS_HEADLESS")) return 0;
    hos_posix_window[0] = hos_android_window != NULL;
    if (hos_android_window) {
        hos_posix_window[1] = ANativeWindow_getWidth(hos_android_window);
        hos_posix_window[2] = ANativeWindow_getHeight(hos_android_window);
    }
    return hos_android_window != NULL;
}
/* the game's thread, when NVN is up (hos_main.c): asks for the renderer; a waiter thread makes it once the
   surface exists (APP_CMD_INIT_WINDOW may still be on its way — the game starts before the first frame can show) */
static DWORD WINAPI renderer_when_window(void *p) {
    (void)p;
    /* the surface can take a moment (rotation, first layout); give up only when the app is gone */
    for (int i = 0; i < 3000 && !hos_android_window; i++) Sleep(10);
    renderer_main(NULL);
    return 0;
}
void hos_display_start(void) {
    if (getenv("HOS_HEADLESS")) {   /* no surface: the renderer still draws, offscreen */
        rx_start(NULL);
        return;
    }
    renderer_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0, renderer_when_window, NULL, 0, NULL));
}

/* android_main's loop calls hos_android_pump itself (it owns the input queue); this stays for shape parity with
   the Linux display — never called on Android */
void hos_display_loop(void) {
    for (;;) Sleep(1000);
}
