package com.bioshock1.recomp

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Surface backed game view. Supports both our standalone Custom GLES 3.2 Xenos
 * renderer (live fractional internal resolution) and ReXGlue Vulkan fallback.
 */
class GameSurfaceView(context: Context, attrs: AttributeSet?) : SurfaceView(context, attrs),
    SurfaceHolder.Callback {

    private var gameRoot: String? = null
    private var driverMode: String = "custom"
    private var internalRes: String = "720p"
    private var vkScale: Int = 1
    private var upscaleMode: String = "lanczos"
    private var fxaa: Boolean = true
    private var perfMode: Boolean = true
    private var initialized = false
    private val mainHandler = Handler(Looper.getMainLooper())
    private val frameRunnable = object : Runnable {
        override fun run() {
            if (initialized) NativeBridge.nativeFrame()
            mainHandler.postDelayed(this, 16)
        }
    }

    init { holder.addCallback(this) }

    fun setGameRoot(path: String) { gameRoot = path }

    fun setRenderConfig(
        driver: String,
        resPreset: String,
        scale: Int,
        upscale: String,
        enableFxaa: Boolean,
        enablePerfMode: Boolean,
    ) {
        driverMode = if (driver == "vulkan") "vulkan" else "custom"
        internalRes = resPreset
        vkScale = scale.coerceIn(1, 2)
        upscaleMode = if (upscale == "bilinear") "bilinear" else "lanczos"
        fxaa = enableFxaa
        perfMode = enablePerfMode
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val root = gameRoot ?: return
        try {
            org.libsdl.app.SDLActivity.bootstrap()
            Log.d("BS1", "SDL bootstrap ok")
        } catch (t: Throwable) {
            Log.e("BS1", "SDL bootstrap failed: ${t.message}")
        }
        org.libsdl.app.SDLActivity.setSurfaceOnly(holder.surface)
        initialized = NativeBridge.nativeInitCustom(
            root, driverMode, internalRes, vkScale, upscaleMode, fxaa, perfMode
        ) == 1
        Log.d(
            "BS1",
            "nativeInitCustom(driver=$driverMode, res=$internalRes, vkScale=${vkScale}x, upscale=$upscaleMode, ${width}x$height) -> $initialized",
        )
        try {
            org.libsdl.app.SDLActivity.surfaceReady(holder.surface, width, height)
            Log.d("BS1", "surfaceReady ok")
        } catch (t: Throwable) {
            Log.e("BS1", "surfaceReady failed: ${t.message}")
        }
        mainHandler.post(frameRunnable)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeBridge.nativeResize(width, height)
        try { org.libsdl.app.SDLActivity.surfaceReady(holder.surface, width, height) }
        catch (t: Throwable) { Log.e("BS1", "surfaceChanged failed: ${t.message}") }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        mainHandler.removeCallbacks(frameRunnable)
        NativeBridge.nativeShutdown()
        initialized = false
    }
}
