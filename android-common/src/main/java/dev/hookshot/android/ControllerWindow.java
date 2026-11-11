package dev.hookshot.android;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.PopupWindow;

/** An activity-owned, non-focusable surface above NativeActivity's Vulkan surface. */
public final class ControllerWindow {
    private final Activity activity;
    public final ControllerOverlay controls;
    private final PopupWindow window;
    private boolean resumed;

    public ControllerWindow(Activity activity, ControllerOverlay.Sink sink) {
        this.activity = activity;
        controls = new ControllerOverlay(activity, sink);
        window = new PopupWindow(controls, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, false);
        window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        window.setTouchable(true);
        window.setTouchModal(false);
        window.setSplitTouchEnabled(false);
        window.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        window.setAnimationStyle(0);
    }
    public void resume() {
        resumed = true;
        activity.getWindow().getDecorView().post(() -> {
            if (!resumed || activity.isFinishing() || activity.isDestroyed()) return;
            if (!window.isShowing()) window.showAtLocation(activity.getWindow().getDecorView(), Gravity.TOP | Gravity.LEFT, 0, 0);
            controls.setActive(true);
            controls.requestApplyInsets();
        });
    }
    public void focus(boolean focus) { controls.setActive(focus && resumed); }
    public void pause() { resumed = false; controls.setActive(false); window.dismiss(); }
}
