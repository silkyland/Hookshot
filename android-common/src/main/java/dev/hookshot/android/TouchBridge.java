package dev.hookshot.android;

/** Native game library is loaded by HookshotActivity before this method is used. */
public final class TouchBridge {
    private TouchBridge() {}
    public static native void publish(long buttons, int lx, int ly, int rx, int ry, boolean enabled);
}
