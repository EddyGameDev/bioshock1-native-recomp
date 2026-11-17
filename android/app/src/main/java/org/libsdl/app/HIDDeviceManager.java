package org.libsdl.app;

/** Auto-generated SDL3 JNI method declaration shim (8 methods). */
class HIDDeviceManager {

    static native void HIDDeviceRegisterCallback();
    static native void HIDDeviceReleaseCallback();
    static native void HIDDeviceConnected(int a0, java.lang.String a1, int a2, int a3, java.lang.String a4, int a5, java.lang.String a6, java.lang.String a7, int a8, int a9, int a10, int a11, boolean a12, int a13);
    static native void HIDDeviceOpenPending(int a0);
    static native void HIDDeviceOpenResult(int a0, boolean a1);
    static native void HIDDeviceDisconnected(int a0);
    static native void HIDDeviceInputReport(int a0, byte[] a1);
    static native void HIDDeviceReportResponse(int a0, byte[] a1);
}
