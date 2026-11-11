package dev.hookshot.android;

public final class ControllerModelTest {
    private static void check(boolean ok, String message) { if (!ok) throw new AssertionError(message); }
    private static ControllerModel.Control find(ControllerModel m, String label) {
        return m.controls().stream().filter(c -> c.label.equals(label) && c.kind == ControllerModel.BUTTON).findFirst().orElseThrow();
    }
    public static void main(String[] args) {
        ControllerModel m = new ControllerModel();
        for (float[] size : new float[][]{{640, 360, 1}, {667, 375, 1}, {3200, 2136, 2.75f}}) {
            m.layout(size[0], size[1], size[2], 0, 0, 0, 24 * size[2]);
            long bits = 0;
            for (ControllerModel.Control c : m.controls()) {
                check(c.width >= 48 * size[2] && c.height >= 48 * size[2], "48dp targets");
                check(c.left >= 0 && c.top >= 0 && c.left + c.width <= size[0] && c.top + c.height <= size[1] - 24 * size[2] + 1, "safe bounds: " + c.label);
                if (c.kind == ControllerModel.BUTTON) {
                    check((bits & (1L << c.bit)) == 0, "duplicate input bit");
                    bits |= 1L << c.bit;
                }
            }
            check(bits == 0x1ffff, "all 17 physical pad inputs");
        }
        m.layout(1280, 720, 1, 0, 0, 0, 24);
        ControllerModel.Control a = find(m, "A"), b = find(m, "B"), up = find(m, "↑");
        check(a.bit == ControllerModel.EAST && b.bit == ControllerModel.SOUTH, "Nintendo default face labels");
        check(a.cx() > b.cx() && a.cy() < b.cy(), "Nintendo face layout");
        ControllerModel.Control left = m.controls().stream().filter(c -> c.kind == ControllerModel.STICK_LEFT).findFirst().orElseThrow();
        ControllerModel.Control right = m.controls().stream().filter(c -> c.kind == ControllerModel.STICK_RIGHT).findFirst().orElseThrow();
        m.down(7, left, left.cx(), left.cy());
        m.move(7, left.cx() + 10000, left.cy() - 10000);
        m.down(18, a, a.cx(), a.cy());
        m.down(3, right, right.cx() - right.width / 2, right.cy());
        ControllerModel.State s = m.state();
        check((s.buttons & (1L << ControllerModel.EAST)) != 0, "button while both sticks held");
        check(s.axes[0] > 0 && s.axes[1] > 0 && s.axes[2] == -32767, "independent radial sticks, up is positive");
        check(Math.hypot(s.axes[0], s.axes[1]) <= 32768, "radial clamp");
        m.up(18);
        check(m.state().buttons == 0 && m.state().axes[0] != 0 && m.state().axes[2] != 0, "lift one pointer only");
        m.down(22, up, up.cx(), up.cy());
        check((m.state().buttons & (1L << ControllerModel.UP)) != 0, "d-pad reachable beside stick");
        m.releaseAll();
        m.down(1, a, a.cx(), a.cy());
        m.down(2, b, b.cx(), b.cy());
        m.move(1, -100, -100);
        check(m.state().buttons == (1L << ControllerModel.SOUTH), "drag off releases only original button");
        m.release(a);
        check(m.state().buttons == (1L << ControllerModel.SOUTH), "cancel one view preserves others");
        m.setVisible(false);
        check(!m.state().enabled && m.state().buttons == 0 && m.state().axes[0] == 0, "hide releases all");
        check(!m.down(0, a, a.cx(), a.cy()), "hidden controls cannot hold input");
        m.setVisible(true);
        check(m.state().enabled && m.state().buttons == 0, "show starts neutral");
        m.down(0, left, left.cx() + 1, left.cy() + 1);
        check(m.state().axes[0] == 0 && m.state().axes[1] == 0, "stick deadzone");
        m.layout(640, 360, 1, 0, 0, 0, 24);
        check(m.state().buttons == 0 && m.state().axes[0] == 0, "resize cancels pointer ownership");
        System.out.println("PASS: layout, multitouch, cancellation, labels and stick bounds");
    }
}
