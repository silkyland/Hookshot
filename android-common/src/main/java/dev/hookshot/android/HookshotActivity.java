package dev.hookshot.android;

import android.app.NativeActivity;
import android.os.Bundle;
import android.view.WindowManager;

/** Java owns touch UI; NativeActivity keeps ownership of its Vulkan surface and input queue. */
public final class HookshotActivity extends NativeActivity {
    private ControllerWindow controller;

    @Override protected void onCreate(Bundle saved) {
        // Register JNI with this class loader as well as NativeActivity's native-code loader.
        System.loadLibrary("hookshot_game");
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        controller = new ControllerWindow(this, state -> TouchBridge.publish(state.buttons,
            state.axes[0], state.axes[1], state.axes[2], state.axes[3], state.enabled));
    }
    @Override protected void onResume() {
        super.onResume(); controller.resume();
    }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (controller != null) controller.focus(focus);
    }
    @Override protected void onPause() {
        controller.pause();
        super.onPause();
    }
    @Override protected void onDestroy() {
        if (controller != null) controller.pause();
        super.onDestroy();
    }
}
