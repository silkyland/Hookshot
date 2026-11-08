#ifndef HOOKSHOT_ANDROID_INPUT_H
#define HOOKSHOT_ANDROID_INPUT_H
/* (android) shared between android_main.c, android_input.c and android_display.c */
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

extern struct android_app *hos_android_app;
extern ANativeWindow *hos_android_window;
extern void hos_android_pump(void);   /* every queued AInputEvent, once */
extern void hos_android_touch_event(AInputEvent *e);   /* android_touch.c */

#endif
