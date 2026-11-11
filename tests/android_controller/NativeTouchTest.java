package dev.hookshot.android;

public final class NativeTouchTest {
    private static native long[] read(boolean physical);
    private static native boolean active();
    private static native void reset();
    private static void check(boolean ok) { if (!ok) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        TouchBridge.publish(-1L, 999999, -999999, Integer.MIN_VALUE, Integer.MAX_VALUE, true);
        long[] state = read(false);
        check(active() && state[0] == (0x1ffffL | (1L << 29)));
        check(state[1] == 32767 && state[2] == -32767 && state[3] == -32767 && state[4] == 32767);
        TouchBridge.publish(1, 100, -100, 100, -100, true);
        state = read(true);
        check(state[1] == 32100 && state[2] == -32100 && state[3] == 12345 && state[4] == -12345);
        TouchBridge.publish(2, 0, 0, 0, 0, false);
        state = read(false);
        check(!active() && state[0] == (1L << 29) && state[1] == 0);
        TouchBridge.publish(1, 1, 2, 3, 4, true);
        Thread writer = new Thread(() -> {
            for (int i = 0; i < 10000; i++) {
                TouchBridge.publish(1, 1, 2, 3, 4, true);
                TouchBridge.publish(2, -1, -2, -3, -4, true);
            }
        });
        writer.start();
        for (int i = 0; i < 10000; i++) {
            state = read(false);
            long bit = state[0] & 3, sign = bit == 1 ? 1 : -1;
            check(bit == 1 || bit == 2);
            for (int j = 1; j <= 4; j++) check(state[j] == sign * j);
        }
        writer.join();
        reset();
        check(!active() && read(false)[1] == 0);
        System.out.println("PASS: real JNI bridge, clamp, physical merge and concurrent snapshots");
    }
}
