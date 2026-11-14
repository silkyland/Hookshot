package dev.hookshot.diagnostics;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import dev.hookshot.android.ControllerModel;
import dev.hookshot.android.ControllerOverlay;
import dev.hookshot.android.ControllerWindow;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;

/** Separate UID/package: exercises shared widgets and the actual bundled decoder, never the game data. */
public final class ProbeActivity extends Activity {
    static { System.loadLibrary("dav1d_bench"); }
    private static native String benchmark(String path, int threads, int delay, int mask);
    private ControllerWindow controller;
    private TextView status;
    private ControllerModel.State last;
    private boolean showControls;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(11, 17, 28));
        status = new TextView(this);
        status.setTextColor(Color.WHITE); status.setTextSize(18); status.setGravity(Gravity.CENTER);
        root.addView(status, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        showControls = getIntent().getBooleanExtra("controls", true);
        controller = new ControllerWindow(this, state -> {
            last = state;
            if (showControls) status.setText(String.format(java.util.Locale.ROOT,
                "Hookshot controller preview\nButtons: %05x\nL: %d, %d   R: %d, %d\n%s",
                state.buttons, state.axes[0], state.axes[1], state.axes[2], state.axes[3],
                state.enabled ? "Controls active" : "Controls released"));
        });
        if (getIntent().getBooleanExtra("selftest", false)) root.postDelayed(this::selftest, 600);
        if (getIntent().getBooleanExtra("benchmark", false)) {
            showControls = false;
            status.setText("Measuring bundled dav1d + NV12 copy\nForeground activity; no game or GPU rendering load");
            int threads = getIntent().getIntExtra("threads", 0);
            int delay = getIntent().getIntExtra("delay", 0);
            int mask = getIntent().getIntExtra("mask", 255);
            root.postDelayed(() -> new Thread(() -> {
                try {
                    JSONObject result = new JSONObject(benchmark(new File(getFilesDir(), "movie.ivf").getPath(), threads, delay, mask));
                    result.put("context", "foreground Activity JNI worker"); result.put("affinity_mask", mask);
                    result.put("cgroup", new String(Files.readAllBytes(new File("/proc/self/cgroup").toPath()), StandardCharsets.UTF_8));
                    write("last.json", result);
                    runOnUiThread(() -> status.setText(result.toString()));
                } catch (Exception error) { failure("last.json", error); }
            }, "BundledAV1Bench").start(), 700);
        }
    }
    @Override protected void onResume() { super.onResume(); if (showControls) controller.resume(); }
    @Override protected void onPause() { controller.pause(); super.onPause(); }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus); if (controller != null) controller.focus(focus);
    }
    private void write(String name, JSONObject data) throws Exception {
        Files.write(new File(getFilesDir(), name).toPath(), data.toString(2).getBytes(StandardCharsets.UTF_8));
    }
    private void failure(String file, Exception error) {
        try { JSONObject data = new JSONObject(); data.put("error", error.toString());
            if (controller != null) {
                ControllerOverlay o = controller.controls;
                data.put("overlay_width", o.getWidth()); data.put("overlay_height", o.getHeight());
                data.put("visible", o.model().visible()); data.put("enabled", last == null ? false : last.enabled);
                org.json.JSONArray children = new org.json.JSONArray();
                for (int i = 0; i < o.getChildCount(); i++) {
                    android.view.View v = o.getChildAt(i);
                    JSONObject child = new JSONObject(); child.put("label", v.getContentDescription());
                    child.put("x", v.getLeft()); child.put("y", v.getTop());
                    child.put("w", v.getWidth()); child.put("h", v.getHeight()); child.put("visibility", v.getVisibility());
                    children.put(child);
                }
                data.put("children", children);
            }
            write(file, data); }
        catch (Exception ignored) { android.util.Log.e("HookshotDiagnostics", "Cannot write result", ignored); }
        runOnUiThread(() -> status.setText(error.toString()));
    }
    private ControllerModel.Control control(int kind, int bit) {
        for (ControllerModel.Control c : controller.controls.model().controls())
            if (c.kind == kind && (kind != ControllerModel.BUTTON || c.bit == bit)) return c;
        throw new IllegalStateException("Missing control " + kind + "/" + bit);
    }
    private void event(int action, int[] ids, float[] x, float[] y) {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[ids.length];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[ids.length];
        for (int i = 0; i < ids.length; i++) {
            properties[i] = new MotionEvent.PointerProperties(); properties[i].id = ids[i];
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords(); coords[i].x = x[i]; coords[i].y = y[i];
            coords[i].pressure = 1; coords[i].size = 1;
        }
        long now = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(now, now, action, ids.length, properties, coords, 0, 0,
            1, 1, 0, 0, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
        controller.controls.dispatchTouchEvent(e); e.recycle();
    }
    private void require(boolean value, String message) { if (!value) throw new IllegalStateException(message); }
    private void selftest() {
        try {
            ControllerOverlay overlay = controller.controls;
            for (int i = 0; i < overlay.getChildCount(); i++) {
                android.view.View child = overlay.getChildAt(i);
                require(child.getWidth() > 0 && child.getHeight() > 0, "Widgets have measured touch bounds");
                require(child.getLeft() >= 0 && child.getTop() >= 0 && child.getRight() <= overlay.getWidth()
                    && child.getBottom() <= overlay.getHeight(), "Widgets fit viewport");
            }
            ControllerModel.Control l = control(ControllerModel.STICK_LEFT, -1);
            ControllerModel.Control a = control(ControllerModel.BUTTON, ControllerModel.EAST);
            event(MotionEvent.ACTION_DOWN, new int[]{0}, new float[]{l.cx()}, new float[]{l.cy()});
            event(MotionEvent.ACTION_MOVE, new int[]{0}, new float[]{l.cx() + l.width * .3f}, new float[]{l.cy()});
            require(last.axes[0] > 10000, "Stick move through real widget");
            event(MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                new int[]{0, 1}, new float[]{l.cx() + l.width * .3f, a.cx()}, new float[]{l.cy(), a.cy()});
            require((last.buttons & (1L << ControllerModel.EAST)) != 0 && last.axes[0] > 10000, "Stick + A simultaneously");
            event(MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                new int[]{0, 1}, new float[]{l.cx() + l.width * .3f, a.cx()}, new float[]{l.cy(), a.cy()});
            require(last.buttons == 0 && last.axes[0] > 10000, "Individual button lift keeps stick");
            event(MotionEvent.ACTION_CANCEL, new int[]{0}, new float[]{l.cx()}, new float[]{l.cy()});
            require(last.buttons == 0 && last.axes[0] == 0, "Cancel releases real widget");
            event(MotionEvent.ACTION_DOWN, new int[]{0}, new float[]{a.cx()}, new float[]{a.cy()});
            controller.focus(false);
            require(last.buttons == 0 && !last.enabled, "Focus loss releases input");
            controller.focus(true);
            event(MotionEvent.ACTION_DOWN, new int[]{0}, new float[]{a.cx()}, new float[]{a.cy()});
            for (int i = 0; i < overlay.model().controls().size(); i++) {
                if (overlay.model().controls().get(i).kind != ControllerModel.TOGGLE) continue;
                overlay.getChildAt(i).performClick();
                require(last.buttons == 0 && !last.enabled, "Hide clears held input");
                overlay.getChildAt(i).performClick();
                require(last.buttons == 0 && last.enabled, "Show starts released");
                break;
            }
            controller.pause(); require(last.buttons == 0 && !last.enabled, "Pause releases input");
            controller.resume();
            JSONObject result = new JSONObject(); result.put("passed", true);
            result.put("checks", "Actual widget dispatch: stick move, multitouch A, individual lift, cancel, focus loss, hide/show, pause");
            write("selftest.json", result);
        } catch (Exception error) { failure("selftest.json", error); }
    }
}
