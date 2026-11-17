package org.libsdl.app;

/** Auto-generated SDL3 JNI method declaration shim (44 methods). */
public class SDLActivity {
    static { System.loadLibrary("SDL3d"); }
    static native java.lang.String nativeGetVersion();
    static native void nativeSetupJNI();
    static native void nativeInitMainThread();
    static native void nativeCleanupMainThread();
    static native int nativeRunMain(java.lang.String a0, java.lang.String a1, java.lang.Object a2);
    static native void onNativeDropFile(java.lang.String a0);
    static native void nativeSetScreenResolution(int a0, int a1, int a2, int a3, float a4, float a5);
    static native void onNativeResize();
    static native void onNativeSurfaceCreated();
    static native void onNativeSurfaceChanged();
    static native void onNativeSurfaceDestroyed();
    static native void onNativeScreenKeyboardShown();
    static native void onNativeScreenKeyboardHidden();
    static native void onNativeKeyDown(int a0);
    static native void onNativeKeyUp(int a0);
    static native boolean onNativeSoftReturnKey();
    static native void onNativeKeyboardFocusLost();
    static native void onNativeTouch(int a0, int a1, int a2, float a3, float a4, float a5);
    static native void onNativePinchStart();
    static native void onNativePinchUpdate(float a0);
    static native void onNativePinchEnd();
    static native void onNativeMouse(int a0, int a1, float a2, float a3, boolean a4);
    static native void onNativePen(int a0, int a1, int a2, int a3, float a4, float a5, float a6);
    static native void onNativeAccel(float a0, float a1, float a2);
    static native void onNativeClipboardChanged();
    static native void nativeLowMemory();
    static native void onNativeLocaleChanged();
    static native void onNativeDarkModeChanged(boolean a0);
    static native void nativeSendQuit();
    static native void nativeQuit();
    static native void nativePause();
    static native void nativeResume();
    static native void nativeFocusChanged(boolean a0);
    static native java.lang.String nativeGetHint(java.lang.String a0);
    static native boolean nativeGetHintBoolean(java.lang.String a0, boolean a1);
    static native void nativeSetenv(java.lang.String a0, java.lang.String a1);
    static native void nativeSetNaturalOrientation(int a0);
    static native void onNativeRotationChanged(int a0);
    static native void onNativeInsetsChanged(int a0, int a1, int a2, int a3);
    static native void nativeAddTouch(int a0, java.lang.String a1);
    static native void nativePermissionResult(int a0, boolean a1);
    static native boolean nativeAllowRecreateActivity();
    static native int nativeCheckSDLThreadCounter();
    static native void onNativeFileDialog(int a0, java.lang.String[] a1, int a2);

    public static String clipboardGetText() { return ""; }
    public static boolean clipboardHasText() { return false; }
    public static void clipboardSetText(String s) {}
    public static int createCustomCursor(int[] a, int b, int c, int d, int e) { return 0; }
    public static void destroyCustomCursor(int a) {}
    private static android.app.Activity mContext;
    public static void setContext(android.app.Activity c) { mContext = c; }
    public static android.app.Activity getContext() { return mContext; }
    public static boolean getManifestEnvironmentVariables() { return false; }
    private static android.view.Surface mSurface;
    public static void setNativeSurface(android.view.Surface s) { mSurface = s; }
    public static android.view.Surface getNativeSurface() { return mSurface; }
    public static void initTouch() {}
    public static boolean isAndroidTV() { return false; }
    public static boolean isChromebook() { return false; }
    public static boolean isDeXMode() { return false; }
    public static boolean isTablet() { return false; }
    public static void manualBackButton() {}
    public static void minimizeWindow() {}
    public static boolean openURL(String s) { return false; }
    public static void requestPermission(String s, int i) {}
    public static boolean showToast(String s, int a, int b, int c, int d) { return false; }
    public static boolean sendMessage(int a, int b) { return false; }
    public static boolean setActivityTitle(String s) { return false; }
    public static boolean setCustomCursor(int a) { return false; }
    public static void setOrientation(int a, int b, boolean c, String d) {}
    public static boolean setRelativeMouseEnabled(boolean b) { return false; }
    public static boolean setSystemCursor(int a) { return false; }
    public static void setWindowStyle(boolean b) {}
    public static boolean shouldMinimizeOnFocusLoss() { return false; }
    public static boolean showTextInput(int a, int b, int c, int d, int e) { return false; }
    public static boolean supportsRelativeMouse() { return false; }
    public static int openFileDescriptor(String a, String b) { return -1; }
    public static boolean showFileDialog(String[] a, boolean b, boolean c, int d) { return false; }
    public static String getPreferredLocales() { return "en-US"; }

    public static void bootstrap() {
        nativeSetupJNI();
        nativeInitMainThread();
        nativeResume();
    }
    public static void setSurfaceOnly(android.view.Surface s) { mSurface = s; }
    public static void surfaceReady(android.view.Surface s, int w, int h) {
        mSurface = s;
        onNativeSurfaceCreated();
        nativeSetScreenResolution(0, 0, w, h, 1f, 1f);
        onNativeSurfaceChanged();
    }
}
