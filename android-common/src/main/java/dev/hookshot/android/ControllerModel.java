package dev.hookshot.android;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** UI-thread controller state. Coordinates are shared by drawing and hit testing. */
public final class ControllerModel {
    // Physical HOS_PAD_* order in gpu/vk/vk_api.h, not Switch nn::hid order.
    public static final int SOUTH = 0, EAST = 1, WEST = 2, NORTH = 3, BACK = 4, GUIDE = 5,
        START = 6, LS = 7, RS = 8, L = 9, R = 10, UP = 11, DOWN = 12, LEFT = 13,
        RIGHT = 14, ZL = 15, ZR = 16;
    public static final int BUTTON = 0, STICK_LEFT = 1, STICK_RIGHT = 2, TOGGLE = 3;

    public static final class Control {
        public final String label, description;
        public final int kind, bit;
        public final float left, top, width, height;
        Control(String label, String description, int kind, int bit, float x, float y, float w, float h) {
            this.label = label; this.description = description; this.kind = kind; this.bit = bit;
            left = x; top = y; width = w; height = h;
        }
        public float cx() { return left + width / 2; }
        public float cy() { return top + height / 2; }
        public boolean contains(float x, float y) {
            return x >= left && x <= left + width && y >= top && y <= top + height;
        }
    }
    public static final class State {
        public long buttons;
        public final int[] axes = new int[4];
        public boolean enabled;
    }
    private static final class Contact {
        final Control control;
        float x, y;
        Contact(Control control, float x, float y) { this.control = control; this.x = x; this.y = y; }
    }
    private final List<Control> controls = new ArrayList<>();
    private final Map<Integer, Contact> contacts = new HashMap<>();
    private boolean visible = true;

    private void button(String label, String description, int bit, float cx, float cy, float size) {
        controls.add(new Control(label, description, BUTTON, bit, cx - size / 2, cy - size / 2, size, size));
    }
    public void layout(float width, float height, float dp, float insetLeft, float insetTop,
                       float insetRight, float insetBottom) {
        releaseAll();
        controls.clear();
        float l = insetLeft + 16 * dp, r = width - insetRight - 16 * dp;
        float t = insetTop + 16 * dp, b = height - insetBottom - 16 * dp;
        float size = 48 * dp, step = 56 * dp;
        for (int i = 0; i < 2; i++) {
            String label = i == 0 ? "L" : "ZL";
            controls.add(new Control(label, "Left " + (i == 0 ? "shoulder" : "trigger"), BUTTON,
                i == 0 ? L : ZL, l + i * 92 * dp, t, 80 * dp, size));
            label = i == 0 ? "R" : "ZR";
            controls.add(new Control(label, "Right " + (i == 0 ? "shoulder" : "trigger"), BUTTON,
                i == 0 ? R : ZR, r - 80 * dp - i * 92 * dp, t, 80 * dp, size));
        }
        float center = (l + r) / 2;
        button("−", "Minus", BACK, center - 60 * dp, t + size / 2, size);
        button("≡", "Hookshot settings", GUIDE, center, t + size / 2, size);
        button("+", "Plus", START, center + 60 * dp, t + size / 2, size);
        controls.add(new Control("Hide", "Hide touch controls", TOGGLE, -1,
            center - 48 * dp, t + 68 * dp, 96 * dp, size));
        controls.add(new Control("L", "Left stick", STICK_LEFT, -1,
            l + 18 * dp, b - 184 * dp, 104 * dp, 104 * dp));
        controls.add(new Control("R", "Right stick", STICK_RIGHT, -1,
            r - 276 * dp, b - 184 * dp, 104 * dp, 104 * dp));
        button("LS", "Click left stick", LS, l + 70 * dp, b - 28 * dp, size);
        button("RS", "Click right stick", RS, r - 224 * dp, b - 28 * dp, size);
        float dx = l + 212 * dp, dy = b - 80 * dp;
        button("↑", "D-pad up", UP, dx, dy - step, size);
        button("↓", "D-pad down", DOWN, dx, dy + step, size);
        button("←", "D-pad left", LEFT, dx - step, dy, size);
        button("→", "D-pad right", RIGHT, dx + step, dy, size);
        float fx = r - 84 * dp, fy = b - 80 * dp;
        // Nintendo face layout, using the host's default physical-pad bindings.
        button("A", "A", EAST, fx + step, fy, size);
        button("B", "B", SOUTH, fx, fy + step, size);
        button("X", "X", NORTH, fx, fy - step, size);
        button("Y", "Y", WEST, fx - step, fy, size);
    }
    public List<Control> controls() { return Collections.unmodifiableList(controls); }
    public boolean visible() { return visible; }
    public void setVisible(boolean on) { releaseAll(); visible = on; }
    public void releaseAll() { contacts.clear(); }
    public void release(Control control) { contacts.values().removeIf(p -> p.control == control); }
    public boolean down(int pointer, Control control, float x, float y) {
        if (!visible || control.kind == TOGGLE || contacts.size() >= 10) return false;
        if (control.kind != BUTTON)
            for (Contact p : contacts.values()) if (p.control == control) return false;
        contacts.put(pointer, new Contact(control, x, y));
        return true;
    }
    public void move(int pointer, float x, float y) {
        Contact p = contacts.get(pointer);
        if (p != null) { p.x = x; p.y = y; }
    }
    public void up(int pointer) { contacts.remove(pointer); }
    public State state() {
        State state = new State();
        state.enabled = visible;
        if (!visible) return state;
        for (Contact p : contacts.values()) {
            Control c = p.control;
            if (c.kind == BUTTON) {
                if (c.contains(p.x, p.y)) state.buttons |= 1L << c.bit;
            } else {
                float radius = c.width / 2;
                float x = (p.x - c.cx()) / radius, y = (c.cy() - p.y) / radius;
                double length = Math.sqrt(x * x + y * y);
                if (length < 0.12) continue;
                if (length > 1) { x /= length; y /= length; }
                int axis = c.kind == STICK_LEFT ? 0 : 2;
                state.axes[axis] = Math.round(x * 32767);
                state.axes[axis + 1] = Math.round(y * 32767);
            }
        }
        return state;
    }
}
