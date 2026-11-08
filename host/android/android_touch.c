/* (android) The touch controls: motion events the pump hands over (android_input.c calls hos_android_touch_event
   for sources that are not gamepads), turned into the same pad state hos_sdl_pads() reports — so the game's
   nn::hid sees touch exactly like a gamepad, and a physical gamepad ORs in on top (stick_max merge).

   Layout (landscape, normalized to the surface; the HUD design pass will replace the constants):
     left 45%          : left stick (drag from its base, radius ~0.12 of the short side)
     right side drag   : camera (right stick), flicks decay
     bottom-right arc  : A (down) B (right) X (left) Y (up) around (0.80, 0.72)
     top edge          : L (left 40%) R (right 40%); corners: minus top-left, plus top-right
     above the stick   : d-pad cross at (0.16, 0.78)... kept clear of the stick's base
     small dots        : L3 under the left stick, R3 under the camera zone
   Every zone is a normalized rect/circle; positions are fixed for now — the settings page will move them. */
#include "../hos.h"
#include <android/input.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#include "../../gpu/vk/vk_api.h"   /* HOS_PAD_* */
#include "android_input.h"

#define TP_MAX 10

typedef struct { int id, kind, pad; float x, y, bx, by; } TP;
static TP tp[TP_MAX];
static int tp_n;
static pthread_mutex_t tp_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tp_buttons;
static int32_t tp_stick[4];
static int tp_cam_x, tp_cam_y;         /* camera drag accumulation, read and cleared by pads */
static int tp_visible = 1;             /* the settings page's Touch controls row */

/* zone kinds */
enum { Z_NONE, Z_STICK_L, Z_STICK_R, Z_BTN, Z_SHOULDER, Z_DPAD, Z_STICKCLICK };
typedef struct { float x, y, r; int pad; } TBtn;   /* circular buttons */
static const TBtn tbtns[] = {
    {0.80f, 0.78f, 0.055f, HOS_PAD_SOUTH},   /* A */
    {0.88f, 0.70f, 0.055f, HOS_PAD_EAST},    /* B */
    {0.72f, 0.70f, 0.055f, HOS_PAD_WEST},    /* X */
    {0.80f, 0.62f, 0.055f, HOS_PAD_NORTH},   /* Y */
    {0.16f, 0.88f, 0.045f, HOS_PAD_LSTICK},  /* L3 */
    {0.90f, 0.50f, 0.045f, HOS_PAD_RSTICK},  /* R3 */
};
static const struct { float x0, x1; int pad; } shoulders[] = {
    {0.00f, 0.40f, HOS_PAD_LSHOULDER}, {0.60f, 1.00f, HOS_PAD_RSHOULDER},
};
/* triggers: a second, higher strip */
static const struct { float x0, x1; int pad; } triggers[] = {
    {0.00f, 0.40f, HOS_PAD_LTRIGGER}, {0.60f, 1.00f, HOS_PAD_RTRIGGER},
};
static const struct { float x, y, r; int pad; } menu_btns[] = {
    {0.965f, 0.06f, 0.035f, HOS_PAD_BACK},   /* minus */
    {0.035f, 0.06f, 0.035f, HOS_PAD_START},  /* plus: swapped vs hardware labels, positions are what matter */
};

static float surf_w = 1, surf_h = 1;
static void norm(float *x, float *y) {   /* pixels -> 0..1, y down */
    *x /= surf_w; *y /= surf_h;
    if (*x < 0) *x = 0; if (*x > 1) *x = 1;
    if (*y < 0) *y = 0; if (*y > 1) *y = 1;
}

static int hit(float x, float y, float cx, float cy, float r) {
    const float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static void recompute(void) {
    uint64_t b = 0;
    int32_t st[4] = {0, 0, 0, 0};
    float lx = 0, ly = 0; int ln = 0;
    for (int i = 0; i < tp_n; i++) {
        TP *t = &tp[i];
        const float x = t->x, y = t->y;
        switch (t->kind) {
        case Z_STICK_L: {
            const float dx = x - t->bx, dy = y - t->by;
            const float max = 0.14f;
            float mx = dx / max, my = dy / max;
            const float m = sqrtf(mx * mx + my * my);
            if (m > 1.0f) { mx /= m; my /= m; }
            lx += mx; ly += my; ln++;
            break;
        }
        case Z_STICK_R:
            st[2] = (int32_t)lrintf((x - t->bx) / 0.14f * 32767.0f);
            st[3] = (int32_t)lrintf(-(y - t->by) / 0.14f * 32767.0f);
            {   /* camera flick -> right stick impulse */
                const float dx = x - t->bx, dy = y - t->by;
                tp_cam_x += (int32_t)lrintf(dx * 40000.0f);
                tp_cam_y += (int32_t)lrintf(-dy * 40000.0f);
            }
            break;
        case Z_BTN:
            for (size_t k = 0; k < sizeof tbtns / sizeof tbtns[0]; k++)
                if (t->pad == tbtns[k].pad && hit(x, y, tbtns[k].x, tbtns[k].y, tbtns[k].r)) b |= 1ULL << t->pad;
            break;
        case Z_SHOULDER:
            for (size_t k = 0; k < sizeof shoulders / sizeof shoulders[0]; k++)
                if (x >= shoulders[k].x0 && x <= shoulders[k].x1 && y < 0.10f) b |= 1ULL << shoulders[k].pad;
            for (size_t k = 0; k < sizeof triggers / sizeof triggers[0]; k++)
                if (x >= triggers[k].x0 && x <= triggers[k].x1 && y >= 0.10f && y < 0.20f)
                    b |= 1ULL << triggers[k].pad;
            for (size_t k = 0; k < sizeof menu_btns / sizeof menu_btns[0]; k++)
                if (hit(x, y, menu_btns[k].x, menu_btns[k].y, menu_btns[k].r)) b |= 1ULL << menu_btns[k].pad;
            break;
        case Z_DPAD:
            /* d-pad at left, above the stick zone's top: a cross centered (0.16, 0.62) */
            if (hit(x, y, 0.16f, 0.55f, 0.05f)) st[1] = -32767;         /* up    */
            if (hit(x, y, 0.16f, 0.75f, 0.05f)) st[1] = 32767;          /* down  */
            if (hit(x, y, 0.08f, 0.65f, 0.05f)) st[0] = -32767;         /* left  */
            if (hit(x, y, 0.24f, 0.65f, 0.05f)) st[0] = 32767;          /* right */
            /* as buttons too (Switch d-pad is buttons, not a stick): set both */
            if (st[1] < 0) b |= 1ULL << HOS_PAD_UP;
            if (st[1] > 0) b |= 1ULL << HOS_PAD_DOWN;
            if (st[0] < 0) b |= 1ULL << HOS_PAD_LEFT;
            if (st[0] > 0) b |= 1ULL << HOS_PAD_RIGHT;
            break;
        }
    }
    if (ln) {
        st[0] = (int32_t)lrintf(lx / ln * 32767.0f);
        st[1] = (int32_t)lrintf(-ly / ln * 32767.0f);
    }
    pthread_mutex_lock(&tp_mu);
    tp_buttons = b;
    memcpy(tp_stick, st, sizeof st);
    pthread_mutex_unlock(&tp_mu);
}

/* called from android_input.c's pump for every motion event (any source; gamepad sources return early there) */
void hos_android_touch_event(AInputEvent *e) {
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return;
    const int32_t action = AMotionEvent_getAction(e);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const int32_t idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    const int down = masked == AMOTION_EVENT_ACTION_DOWN || masked == AMOTION_EVENT_ACTION_POINTER_DOWN;
    const int up = masked == AMOTION_EVENT_ACTION_UP || masked == AMOTION_EVENT_ACTION_POINTER_UP ||
                   masked == AMOTION_EVENT_ACTION_CANCEL;

    pthread_mutex_lock(&tp_mu);
    if (down) {
        float x = AMotionEvent_getX(e, (size_t)idx), y = AMotionEvent_getY(e, (size_t)idx);
        const int w = ANativeWindow_getWidth(hos_android_window);
        const int h = ANativeWindow_getHeight(hos_android_window);
        surf_w = w > 0 ? (float)w : 1; surf_h = h > 0 ? (float)h : 1;
        norm(&x, &y);
        if (tp_n < TP_MAX) {
            TP *t = &tp[tp_n++];
            memset(t, 0, sizeof *t);
            t->id = AMotionEvent_getPointerId(e, (size_t)idx);
            t->x = t->bx = x; t->y = t->by = y;
            /* classify by where it landed */
            if (x < 0.45f) t->kind = Z_STICK_L;
            else if (y < 0.20f) t->kind = Z_SHOULDER;
            else if (x < 0.30f && y > 0.50f) t->kind = Z_DPAD;
            else if (hit(x, y, 0.16f, 0.65f, 0.09f) || hit(x, y, 0.16f, 0.55f, 0.09f) ||
                     hit(x, y, 0.16f, 0.75f, 0.09f) || hit(x, y, 0.08f, 0.65f, 0.09f) ||
                     hit(x, y, 0.24f, 0.65f, 0.09f)) t->kind = Z_DPAD;
            else {
                int near_btn = 0;
                for (size_t k = 0; k < sizeof tbtns / sizeof tbtns[0]; k++)
                    if (hit(x, y, tbtns[k].x, tbtns[k].y, tbtns[k].r * 1.8f)) { t->kind = Z_BTN; t->pad = tbtns[k].pad; near_btn = 1; break; }
                if (!near_btn) {
                    for (size_t k = 0; k < sizeof menu_btns / sizeof menu_btns[0]; k++)
                        if (hit(x, y, menu_btns[k].x, menu_btns[k].y, menu_btns[k].r * 1.8f)) { t->kind = Z_SHOULDER; break; }
                    if (t->kind == Z_NONE) t->kind = (x > 0.55f && y > 0.35f) ? Z_STICK_R : Z_STICK_L;
                }
            }
        }
    } else if (up || masked == AMOTION_EVENT_ACTION_MOVE) {
        /* find the pointer (for MOVE, all pointers move; update each tracked id) */
        const int count = AMotionEvent_getPointerCount(e);
        for (int i = 0; i < tp_n; i++) {
            for (int p = 0; p < count; p++) {
                if (AMotionEvent_getPointerId(e, (size_t)p) != tp[i].id) continue;
                if (masked == AMOTION_EVENT_ACTION_MOVE || p == idx || up) {
                    float x = AMotionEvent_getX(e, (size_t)p), y = AMotionEvent_getY(e, (size_t)p);
                    const int w = ANativeWindow_getWidth(hos_android_window);
                    const int h = ANativeWindow_getHeight(hos_android_window);
                    surf_w = w > 0 ? (float)w : 1; surf_h = h > 0 ? (float)h : 1;
                    norm(&x, &y);
                    tp[i].x = x; tp[i].y = y;
                }
            }
        }
        if (up) {
            for (int i = 0; i < tp_n; i++)
                if (tp[i].id == AMotionEvent_getPointerId(e, (size_t)(masked == AMOTION_EVENT_ACTION_UP ? 0 : idx))) {
                    tp[i] = tp[--tp_n];
                    i--;
                }
        }
    }
    recompute();
    pthread_mutex_unlock(&tp_mu);
}

/* what android_input.c's hos_sdl_pads() merges in */
int hos_touch_active(void) { return tp_visible && tp_n > 0; }
void hos_touch_pads(uint64_t *b, int32_t s[4]) {
    pthread_mutex_lock(&tp_mu);
    if (tp_visible) {
        *b |= tp_buttons;
        for (int i = 0; i < 4; i++) {
            const int32_t v = tp_stick[i];
            if ((v < 0 ? -v : v) > (s[i] < 0 ? -s[i] : s[i])) s[i] = v;
        }
        s[2] += tp_cam_x; s[3] += tp_cam_y;
        tp_cam_x = tp_cam_x * 3 / 4;   /* the flick decays over a few reads */
        tp_cam_y = tp_cam_y * 3 / 4;
    }
    pthread_mutex_unlock(&tp_mu);
}
