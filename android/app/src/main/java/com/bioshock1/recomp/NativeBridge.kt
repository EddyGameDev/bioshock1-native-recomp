package com.bioshock1.recomp

/** JNI bridge to the real-runtime libbioshock1-android.so. */
object NativeBridge {
    init {
        // The APK ships many ReXGlue .so modules; android:extractNativeLibs is
        // false, so the loader resolves them from inside the APK by SONAME.
        System.loadLibrary("bioshock1-android")
    }

    external fun nativeInit(gameRoot: String, width: Int, height: Int): Int
    external fun nativeShutdown()
    external fun nativeResize(width: Int, height: Int)
    external fun nativeRender()
    external fun nativeFrame(): Int
    external fun nativeSetControls(axes: FloatArray, buttons: ByteArray)
    external fun nativeSubmitCmds(buffer: ByteArray): Int
    external fun nativeStatus(): String
    external fun nativeHleImportCount(): Int
}
