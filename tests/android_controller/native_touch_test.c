#include <jni.h>
#include <stdint.h>
void hos_touch_pads(uint64_t *, int32_t[4]);
int hos_touch_active(void);
void hos_touch_reset(void);
JNIEXPORT jlongArray JNICALL Java_dev_hookshot_android_NativeTouchTest_read(JNIEnv *env, jclass type, jboolean physical) {
    (void)type;
    uint64_t buttons = 1ULL << 29;
    int32_t axes[4] = {0};
    if (physical) { axes[0] = 32100; axes[1] = -32100; axes[2] = 12345; axes[3] = -12345; }
    hos_touch_pads(&buttons, axes);
    const jlong values[5] = {(jlong)buttons, axes[0], axes[1], axes[2], axes[3]};
    jlongArray result = (*env)->NewLongArray(env, 5);
    (*env)->SetLongArrayRegion(env, result, 0, 5, values);
    return result;
}
JNIEXPORT jboolean JNICALL Java_dev_hookshot_android_NativeTouchTest_active(JNIEnv *env, jclass type) {
    (void)env; (void)type; return hos_touch_active() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_dev_hookshot_android_NativeTouchTest_reset(JNIEnv *env, jclass type) {
    (void)env; (void)type; hos_touch_reset();
}
