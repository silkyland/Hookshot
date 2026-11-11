/* Android overlay state, published from Java's UI thread and sampled by nn::hid.
   Pointer ownership and drawing live in android-common; native code holds one coherent snapshot.
   No Android event processing or drawing runs on the game's threads. */
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

static pthread_mutex_t touch_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t touch_buttons;
static int32_t touch_sticks[4];
static int touch_enabled;

static int32_t clamp_axis(jint v) { return v < -32767 ? -32767 : v > 32767 ? 32767 : v; }

JNIEXPORT void JNICALL Java_dev_hookshot_android_TouchBridge_publish(
    JNIEnv *env, jclass type, jlong buttons, jint lx, jint ly, jint rx, jint ry, jboolean enabled) {
    (void)env; (void)type;
    pthread_mutex_lock(&touch_mu);
    touch_enabled = enabled != JNI_FALSE;
    touch_buttons = touch_enabled ? (uint64_t)buttons & 0x1ffffULL : 0; /* HOS_PAD_*: 17 physical inputs */
    touch_sticks[0] = touch_enabled ? clamp_axis(lx) : 0;
    touch_sticks[1] = touch_enabled ? clamp_axis(ly) : 0;
    touch_sticks[2] = touch_enabled ? clamp_axis(rx) : 0;
    touch_sticks[3] = touch_enabled ? clamp_axis(ry) : 0;
    pthread_mutex_unlock(&touch_mu);
}

void hos_touch_reset(void) {
    pthread_mutex_lock(&touch_mu);
    touch_enabled = 0;
    touch_buttons = 0;
    memset(touch_sticks, 0, sizeof touch_sticks);
    pthread_mutex_unlock(&touch_mu);
}
int hos_touch_active(void) {
    pthread_mutex_lock(&touch_mu);
    const int enabled = touch_enabled;
    pthread_mutex_unlock(&touch_mu);
    return enabled;
}
void hos_touch_pads(uint64_t *buttons, int32_t axes[4]) {
    pthread_mutex_lock(&touch_mu);
    *buttons |= touch_buttons;
    for (int i = 0; i < 4; i++) {
        const int64_t incoming = touch_sticks[i], current = axes[i];
        if ((incoming < 0 ? -incoming : incoming) > (current < 0 ? -current : current))
            axes[i] = touch_sticks[i];
    }
    pthread_mutex_unlock(&touch_mu);
}
