package org.libsdl.app;

/** Auto-generated SDL3 JNI method declaration shim (10 methods). */
class SDLControllerManager {

    static native void nativeSetupJNI();
    static native boolean onNativePadDown(int a0, int a1, int a2);
    static native boolean onNativePadUp(int a0, int a1, int a2);
    static native void onNativeJoy(int a0, int a1, float a2);
    static native void onNativeHat(int a0, int a1, int a2, int a3);
    static native void onNativeJoySensor(int a0, int a1, long a2, float a3, float a4, float a5);
    static native void nativeAddJoystick(int a0, java.lang.String a1, java.lang.String a2, int a3, int a4, int a5, int a6, int a7, int a8, boolean a9, boolean a10, boolean a11, boolean a12);
    static native void nativeRemoveJoystick(int a0);
    static native void nativeAddHaptic(int a0, java.lang.String a1);
    static native void nativeRemoveHaptic(int a0);
}
