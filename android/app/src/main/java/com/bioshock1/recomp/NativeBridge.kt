package com.bioshock1.recomp

/** JNI bridge to the real-runtime libbioshock1-android.so. */
object NativeBridge {
    init {
        System.loadLibrary("bioshock1-android")
    }

    external fun nativeInit(gameRoot: String, width: Int, height: Int): Int
    external fun nativeInitWithConfig(
        gameRoot: String,
        resScale: Int,
        upscaleMode: String,
        fxaa: Boolean,
        perfMode: Boolean,
    ): Int
    external fun nativeInitCustom(
        gameRoot: String,
        driverMode: String,
        internalRes: String,
        vkScale: Int,
        upscaleMode: String,
        fxaa: Boolean,
        perfMode: Boolean,
    ): Int
    external fun nativeSetInternalResolution(preset: String): Boolean
    external fun nativeSetUpscaleMode(upscaleMode: String): Boolean
    external fun nativeGetPresentCount(): Long
    external fun nativeShutdown()
    external fun nativeResize(width: Int, height: Int)
    external fun nativeRender()
    external fun nativeFrame(): Int
    external fun nativeSetControls(axes: FloatArray, buttons: ByteArray)
    external fun nativeSubmitCmds(buffer: ByteArray): Int
    external fun nativeStatus(): String
    external fun nativeHleImportCount(): Int
}
