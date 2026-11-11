/* (android) Input: the AInputQueue's key and motion events, dispatched by native_app_glue on the android_main
   thread, become (a) hos_posix_keys[] (Windows VK codes: nn_hid.c's keyboard binds, the Insert menu's keys) and
   (b) the gamepad state hos_sdl_pads() reports to nn_hid.c's pads_read(): buttons as 1 << HOS_PAD_* positions and
   sticks as int32_t +-32767, exactly what pad_sdl.c produces on Linux. Touch overlay input lands in the same
   places (android_touch.c feeds the same state).

   Gamepad events: keycodes AKEYCODE_BUTTON_* (buttons) and DPAD; motion axes AMOTION_EVENT_AXIS_* (sticks,
   triggers, hat). Axis ranges are normalized per InputDevice's motion ranges where the event carries them; the
   common case (-1..1 floats, triggers 0..1) is handled directly. */
#include "../hos.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <math.h>
#include <string.h>
#include <windows.h>

#include "../../gpu/vk/vk_api.h"   /* HOS_PAD_*, HOS_SW_* */
#include "android_input.h"

#ifndef VK_LWIN
#define VK_LWIN 0x5B
#endif
extern volatile uint8_t hos_posix_keys[256];   /* android_display.c: the keyboard state GetAsyncKeyState reads */

extern struct android_app *hos_android_app;

/* ---- the pad state, shared with the pump (written here, read by pads_read on the game thread) ---- */
static pthread_mutex_t in_mu = PTHREAD_MUTEX_INITIALIZER;
static int32_t pad_stick[4];            /* LX LY RX RY, +-32767 */
static int pad_connected;
static uint64_t key_buttons;            /* gamepad buttons held, by HOS_PAD_* (from key events) */
static float axis_val[32];              /* last motion axis values, indexed by AMOTION_EVENT_AXIS_* */

static const struct { int key; int pad; } key_map[] = {
    {AKEYCODE_BUTTON_A, HOS_PAD_SOUTH},   {AKEYCODE_BUTTON_B, HOS_PAD_EAST},
    {AKEYCODE_BUTTON_X, HOS_PAD_WEST},    {AKEYCODE_BUTTON_Y, HOS_PAD_NORTH},
    {AKEYCODE_BUTTON_L1, HOS_PAD_LSHOULDER}, {AKEYCODE_BUTTON_R1, HOS_PAD_RSHOULDER},
    {AKEYCODE_BUTTON_L2, HOS_PAD_LTRIGGER},  {AKEYCODE_BUTTON_R2, HOS_PAD_RTRIGGER},
    {AKEYCODE_BUTTON_THUMBL, HOS_PAD_LSTICK}, {AKEYCODE_BUTTON_THUMBR, HOS_PAD_RSTICK},
    {AKEYCODE_BUTTON_START, HOS_PAD_START},  {AKEYCODE_BUTTON_SELECT, HOS_PAD_BACK},
    {AKEYCODE_BUTTON_MODE, HOS_PAD_GUIDE},
    {AKEYCODE_DPAD_UP, HOS_PAD_UP}, {AKEYCODE_DPAD_DOWN, HOS_PAD_DOWN},
    {AKEYCODE_DPAD_LEFT, HOS_PAD_LEFT}, {AKEYCODE_DPAD_RIGHT, HOS_PAD_RIGHT},
};

/* ---- keys: Android keycodes as Windows VK codes (hos_posix_keys, GetAsyncKeyState) ---- */
static int vk_of_keycode(int code) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return 'A' + (code - AKEYCODE_A);
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) return '0' + (code - AKEYCODE_0);
    switch (code) {
    case AKEYCODE_ENTER: case AKEYCODE_NUMPAD_ENTER: return VK_RETURN;
    case AKEYCODE_DEL: return VK_BACK;
    case AKEYCODE_FORWARD_DEL: return VK_DELETE;
    case AKEYCODE_TAB: return VK_TAB;
    case AKEYCODE_SPACE: return VK_SPACE;
    case AKEYCODE_ESCAPE: return VK_ESCAPE;
    case AKEYCODE_DPAD_UP: return VK_UP; case AKEYCODE_DPAD_DOWN: return VK_DOWN;
    case AKEYCODE_DPAD_LEFT: return VK_LEFT; case AKEYCODE_DPAD_RIGHT: return VK_RIGHT;
    case AKEYCODE_MOVE_HOME: return VK_HOME; case AKEYCODE_MOVE_END: return VK_END;
    case AKEYCODE_PAGE_UP: return VK_PRIOR; case AKEYCODE_PAGE_DOWN: return VK_NEXT;
    case AKEYCODE_INSERT: return VK_INSERT;
    case AKEYCODE_F1: case AKEYCODE_F2: case AKEYCODE_F3: case AKEYCODE_F4:
    case AKEYCODE_F5: case AKEYCODE_F6: case AKEYCODE_F7: case AKEYCODE_F8:
    case AKEYCODE_F9: case AKEYCODE_F10: case AKEYCODE_F11: case AKEYCODE_F12:
        return VK_F1 + (code - AKEYCODE_F1);
    case AKEYCODE_SHIFT_LEFT: case AKEYCODE_SHIFT_RIGHT: return VK_SHIFT;
    case AKEYCODE_CTRL_LEFT: case AKEYCODE_CTRL_RIGHT: return VK_CONTROL;
    case AKEYCODE_ALT_LEFT: case AKEYCODE_ALT_RIGHT: return VK_MENU;
    case AKEYCODE_META_LEFT: case AKEYCODE_META_RIGHT: return VK_LWIN;
    case AKEYCODE_NUMPAD_0: case AKEYCODE_NUMPAD_1: case AKEYCODE_NUMPAD_2:
    case AKEYCODE_NUMPAD_3: case AKEYCODE_NUMPAD_4: case AKEYCODE_NUMPAD_5:
    case AKEYCODE_NUMPAD_6: case AKEYCODE_NUMPAD_7: case AKEYCODE_NUMPAD_8:
    case AKEYCODE_NUMPAD_9: return VK_NUMPAD0 + (code - AKEYCODE_NUMPAD_0);
    case AKEYCODE_NUMPAD_ADD: return VK_ADD; case AKEYCODE_NUMPAD_SUBTRACT: return VK_SUBTRACT;
    case AKEYCODE_NUMPAD_MULTIPLY: return VK_MULTIPLY; case AKEYCODE_NUMPAD_DIVIDE: return VK_DIVIDE;
    case AKEYCODE_NUMPAD_DOT: return VK_DECIMAL;
    case AKEYCODE_MINUS: return VK_OEM_MINUS; case AKEYCODE_EQUALS: return VK_OEM_PLUS;
    case AKEYCODE_COMMA: return VK_OEM_COMMA; case AKEYCODE_PERIOD: return VK_OEM_PERIOD;
    case AKEYCODE_SLASH: return VK_OEM_2; case AKEYCODE_SEMICOLON: return VK_OEM_1;
    case AKEYCODE_APOSTROPHE: return VK_OEM_7; case AKEYCODE_GRAVE: return VK_OEM_3;
    case AKEYCODE_LEFT_BRACKET: return VK_OEM_4; case AKEYCODE_RIGHT_BRACKET: return VK_OEM_6;
    case AKEYCODE_BACKSLASH: return VK_OEM_5;
    }
    return 0;
}

static int key_event(int code, int down) {
    const int vk = vk_of_keycode(code);
    if (vk && vk < 256) hos_posix_keys[vk] = down ? 1 : 0;
    int handled = vk != 0;
    pthread_mutex_lock(&in_mu);
    for (size_t i = 0; i < sizeof key_map / sizeof key_map[0]; i++)
        if (key_map[i].key == code) {
            handled = 1;
            if (down) key_buttons |= 1ULL << key_map[i].pad;
            else key_buttons &= ~(1ULL << key_map[i].pad);
            pad_connected = 1;
        }
    pthread_mutex_unlock(&in_mu);
    return handled;
}

/* ---- motion: the Java overlay owns touchscreen pointers; native code reads physical gamepads ---- */
static int32_t axis_to_stick(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    const int32_t s = (int32_t)lrintf(v * 32767.0f);
    return (s > 7849 || s < -7849) ? s : 0;   /* pad_sdl.c's dead zone */
}
static void motion_event(AInputEvent *e) {
    const int32_t source = AInputEvent_getSource(e);
    if (!(source & AINPUT_SOURCE_GAMEPAD) && !(source & AINPUT_SOURCE_JOYSTICK) &&
        !(source & AINPUT_SOURCE_DPAD))
        return;   /* touch/screen pens: the overlay's own path */
    /* axes: pointer 0 carries them */
    for (int a = 0; a < 32; a++) {
        float v = AMotionEvent_getAxisValue(e, a, 0);
        if (v != 0.0f || axis_val[a] != 0.0f) axis_val[a] = v;
    }
    pthread_mutex_lock(&in_mu);
    pad_connected = 1;
    /* sticks: X/Y left, Z/RZ right (the common Android gamepad layout; per-device quirks are a later mapping) */
    pad_stick[0] = axis_to_stick(axis_val[AMOTION_EVENT_AXIS_X]);
    pad_stick[1] = axis_to_stick(-axis_val[AMOTION_EVENT_AXIS_Y]);   /* screen Y is down, stick Y is up */
    pad_stick[2] = axis_to_stick(axis_val[AMOTION_EVENT_AXIS_Z]);
    pad_stick[3] = axis_to_stick(-axis_val[AMOTION_EVENT_AXIS_RZ]);
    /* hat (dpad on pads that report it as axes) */
    const float hx = axis_val[AMOTION_EVENT_AXIS_HAT_X], hy = axis_val[AMOTION_EVENT_AXIS_HAT_Y];
    if (hx < -0.5f) key_buttons |= 1ULL << HOS_PAD_LEFT; else key_buttons &= ~(1ULL << HOS_PAD_LEFT);
    if (hx > 0.5f) key_buttons |= 1ULL << HOS_PAD_RIGHT; else key_buttons &= ~(1ULL << HOS_PAD_RIGHT);
    if (hy < -0.5f) key_buttons |= 1ULL << HOS_PAD_UP; else key_buttons &= ~(1ULL << HOS_PAD_UP);
    if (hy > 0.5f) key_buttons |= 1ULL << HOS_PAD_DOWN; else key_buttons &= ~(1ULL << HOS_PAD_DOWN);
    /* triggers as axes (0..1 -> the 8000/32767 threshold pad_sdl.c uses is ~0.24) */
    if (axis_val[AMOTION_EVENT_AXIS_LTRIGGER] > 0.24f || axis_val[AMOTION_EVENT_AXIS_BRAKE] > 0.24f)
        key_buttons |= 1ULL << HOS_PAD_LTRIGGER;
    else key_buttons &= ~(1ULL << HOS_PAD_LTRIGGER);
    if (axis_val[AMOTION_EVENT_AXIS_RTRIGGER] > 0.24f || axis_val[AMOTION_EVENT_AXIS_GAS] > 0.24f)
        key_buttons |= 1ULL << HOS_PAD_RTRIGGER;
    else key_buttons &= ~(1ULL << HOS_PAD_RTRIGGER);
    pthread_mutex_unlock(&in_mu);
}

static void stick_max_local(int32_t *dst, const int32_t *src) {
    for (int i = 0; i < 4; i++)
        if ((src[i] < 0 ? -src[i] : src[i]) > (dst[i] < 0 ? -dst[i] : dst[i])) dst[i] = src[i];
}

int32_t hos_android_input_event(struct android_app *app, AInputEvent *e) {
    (void)app;
    const int32_t type = AInputEvent_getType(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int action = AKeyEvent_getAction(e);
        return key_event(AKeyEvent_getKeyCode(e), action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_MULTIPLE);
    }
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        const int source = AInputEvent_getSource(e);
        if ((source & AINPUT_SOURCE_GAMEPAD) || (source & AINPUT_SOURCE_JOYSTICK) || (source & AINPUT_SOURCE_DPAD)) {
            motion_event(e);
            return 1;
        }
    }
    return 0;
}
void hos_android_input_reset(void) {
    pthread_mutex_lock(&in_mu);
    key_buttons = 0;
    memset(pad_stick, 0, sizeof pad_stick);
    memset(axis_val, 0, sizeof axis_val);
    memset((void *)hos_posix_keys, 0, 256);
    pthread_mutex_unlock(&in_mu);
    hos_touch_reset();
}

/* ---- what nn_hid.c's pads_read() calls (pad_sdl.c's place on Android) ----
   Returns the merged button bits (1 << HOS_PAD_*) and sticks; the count is 1 while any gamepad or the touch
   overlay is active (the count only feeds a log line). */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]) {
    extern int hos_touch_active(void);          /* android_touch.c: the overlay's own state, OR'd in */
    extern void hos_touch_pads(uint64_t *b, int32_t s[4]);
    uint64_t tb = 0;
    int32_t ts[4] = {0};
    pthread_mutex_lock(&in_mu);
    uint64_t b = key_buttons;
    int32_t s[4] = {pad_stick[0], pad_stick[1], pad_stick[2], pad_stick[3]};
    const int conn = pad_connected;
    pthread_mutex_unlock(&in_mu);
    hos_touch_pads(&tb, ts);
    b |= tb;
    stick_max_local(s, ts);
    *buttons |= b;
    stick_max_local(stick, s);
    return conn || hos_touch_active() ? 1 : 0;
}
void hos_sdl_pads_pump(void) {}   /* native_app_glue dispatches on its own thread */
void hos_sdl_rumble(float low, float high) { (void)low, (void)high; }   /* no standard Android gamepad FF */
