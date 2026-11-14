package dev.hookshot.android;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.Button;
import android.widget.FrameLayout;
import java.util.ArrayList;
import java.util.List;

/** Native widgets in a separate PopupWindow, never painted into the game's Vulkan surface. */
public final class ControllerOverlay extends FrameLayout {
    public interface Sink { void publish(ControllerModel.State state); }
    private static final int IDLE = 0xb018202d, PRESSED = 0xed62dfbb, BORDER = 0xdde8f2ff;
    private final ControllerModel model = new ControllerModel();
    private final Sink sink;
    private final List<View> pads = new ArrayList<>();
    private Button toggle;
    private int insetLeft, insetTop, insetRight, insetBottom;
    private boolean active = true;
    private int layoutWidth = -1, layoutHeight = -1;
    private boolean layoutDirty = true;

    public ControllerOverlay(Context context, Sink sink) {
        super(context);
        this.sink = sink;
        setMotionEventSplittingEnabled(true);
        setClipChildren(false);
        setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            if (insetLeft != i.left || insetTop != i.top || insetRight != i.right || insetBottom != i.bottom) {
                insetLeft = i.left; insetTop = i.top; insetRight = i.right; insetBottom = i.bottom;
                layoutDirty = true;
                requestLayout();
            }
            return insets;
        });
    }
    public ControllerModel model() { return model; }
    private float dp() { return getResources().getDisplayMetrics().density; }
    @Override protected void onMeasure(int widthSpec, int heightSpec) {
        int width = MeasureSpec.getSize(widthSpec), height = MeasureSpec.getSize(heightSpec);
        if (width > 0 && height > 0 && (layoutDirty || width != layoutWidth || height != layoutHeight)) {
            layoutWidth = width; layoutHeight = height; layoutDirty = false;
            // Children must exist before FrameLayout measures them. onSizeChanged runs after measurement.
            rebuild(width, height);
        }
        super.onMeasure(widthSpec, heightSpec);
    }
    private void publish() {
        ControllerModel.State s = model.state();
        if (!active) { s.buttons = 0; java.util.Arrays.fill(s.axes, 0); s.enabled = false; }
        sink.publish(s);
        for (View pad : pads) {
            if (pad instanceof PadButton) pad.setPressed((s.buttons & (1L << ((PadButton)pad).control.bit)) != 0);
            else pad.invalidate();
        }
    }
    public void setActive(boolean on) { active = on; model.releaseAll(); publish(); }
    public void releaseAll() { model.releaseAll(); publish(); }
    private void rebuild(int width, int height) {
        model.layout(width, height, dp(), insetLeft, insetTop, insetRight, insetBottom);
        removeAllViews();
        pads.clear();
        for (ControllerModel.Control c : model.controls()) {
            View view;
            if (c.kind == ControllerModel.TOGGLE) {
                toggle = new Button(getContext());
                styleButton(toggle, c);
                toggle.setOnClickListener(v -> {
                    model.setVisible(!model.visible());
                    updateVisibility();
                    publish();
                });
                view = toggle;
            } else {
                view = c.kind == ControllerModel.BUTTON ? new PadButton(c) : new Stick(c);
                pads.add(view);
            }
            LayoutParams params = new LayoutParams(Math.round(c.width), Math.round(c.height));
            params.leftMargin = Math.round(c.left); params.topMargin = Math.round(c.top);
            addView(view, params);
        }
        updateVisibility();
        publish();
    }
    private void updateVisibility() {
        for (View p : pads) p.setVisibility(model.visible() ? VISIBLE : INVISIBLE);
        toggle.setText(model.visible() ? "Hide" : "Controls");
        toggle.setContentDescription(model.visible() ? "Hide touch controls" : "Show touch controls");
    }
    private GradientDrawable background(int color, boolean round) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color); d.setCornerRadius((round ? 28 : 16) * dp());
        d.setStroke(Math.max(1, Math.round(dp())), BORDER);
        return d;
    }
    private void styleButton(Button b, ControllerModel.Control c) {
        StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{android.R.attr.state_pressed}, background(PRESSED, c.width == c.height));
        states.addState(new int[]{}, background(IDLE, c.width == c.height));
        b.setBackground(states);
        b.setText(c.label); b.setAllCaps(false);
        b.setTextColor(new ColorStateList(new int[][]{new int[]{android.R.attr.state_pressed}, new int[]{}},
            new int[]{0xff071e18, Color.WHITE}));
        b.setContentDescription(c.description);
        b.setPadding(4, 0, 4, 0);
        b.setMinWidth(0); b.setMinHeight(0); b.setMinimumWidth(0); b.setMinimumHeight(0);
        b.setAutoSizeTextTypeUniformWithConfiguration(12, 22, 1, android.util.TypedValue.COMPLEX_UNIT_SP);
        b.setStateListAnimator(null);
    }
    private void handle(ControllerModel.Control c, View view, MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_CANCEL) model.release(c);
        else if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN)
            model.down(event.getPointerId(index), c, c.left + event.getX(index), c.top + event.getY(index));
        else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
            model.up(event.getPointerId(index));
        else if (action == MotionEvent.ACTION_MOVE)
            for (int i = 0; i < event.getPointerCount(); i++)
                model.move(event.getPointerId(i), c.left + event.getX(i), c.top + event.getY(i));
        publish();
    }
    private void pulse(ControllerModel.Control c, float x, float y) {
        final int owner = -100 - model.controls().indexOf(c);
        model.down(owner, c, x, y); publish();
        postDelayed(() -> { model.up(owner); publish(); }, 150);
    }
    private final class PadButton extends Button {
        final ControllerModel.Control control;
        PadButton(ControllerModel.Control c) { super(ControllerOverlay.this.getContext()); control = c; styleButton(this, c); }
        @Override public boolean onTouchEvent(MotionEvent e) {
            handle(control, this, e);
            if (e.getActionMasked() == MotionEvent.ACTION_UP) super.performClick();
            return true;
        }
        @Override public boolean performClick() { pulse(control, control.cx(), control.cy()); super.performClick(); return true; }
    }
    private final class Stick extends View {
        private final ControllerModel.Control control;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private static final int MOVE_LEFT = 0x01010001, MOVE_UP = 0x01010002,
            MOVE_RIGHT = 0x01010003, MOVE_DOWN = 0x01010004;
        Stick(ControllerModel.Control c) {
            super(ControllerOverlay.this.getContext()); control = c;
            setContentDescription(c.description + ". Drag to move."); setFocusable(true);
            setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        }
        @Override public boolean onTouchEvent(MotionEvent e) { handle(control, this, e); return true; }
        @Override protected void onDraw(Canvas canvas) {
            float x = getWidth() / 2f, y = getHeight() / 2f, radius = Math.min(x, y) - dp();
            paint.setStyle(Paint.Style.FILL); paint.setColor(IDLE); canvas.drawCircle(x, y, radius, paint);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(dp()); paint.setColor(BORDER);
            canvas.drawCircle(x, y, radius, paint);
            ControllerModel.State state = model.state(); int a = control.kind == ControllerModel.STICK_LEFT ? 0 : 2;
            float travel = radius * .52f;
            paint.setStyle(Paint.Style.FILL); paint.setColor(state.axes[a] != 0 || state.axes[a + 1] != 0 ? PRESSED : 0xcce8f2ff);
            canvas.drawCircle(x + state.axes[a] / 32767f * travel, y - state.axes[a + 1] / 32767f * travel, radius * .42f, paint);
        }
        @Override public void onInitializeAccessibilityNodeInfo(AccessibilityNodeInfo info) {
            super.onInitializeAccessibilityNodeInfo(info);
            info.addAction(new AccessibilityNodeInfo.AccessibilityAction(MOVE_LEFT, "Move left"));
            info.addAction(new AccessibilityNodeInfo.AccessibilityAction(MOVE_UP, "Move up"));
            info.addAction(new AccessibilityNodeInfo.AccessibilityAction(MOVE_RIGHT, "Move right"));
            info.addAction(new AccessibilityNodeInfo.AccessibilityAction(MOVE_DOWN, "Move down"));
        }
        @Override public boolean performAccessibilityAction(int action, Bundle args) {
            float x = control.cx(), y = control.cy(), r = control.width / 2;
            if (action == MOVE_LEFT) x -= r;
            else if (action == MOVE_RIGHT) x += r;
            else if (action == MOVE_UP) y -= r;
            else if (action == MOVE_DOWN) y += r;
            else return super.performAccessibilityAction(action, args);
            pulse(control, x, y); return true;
        }
    }
}
