package com.bioshock1.recomp

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * Surface backed game view. The ReXGlue runtime renders with Vulkan into the
 * ANativeWindow. SDL3 requires RunFrame()/SDL_PumpEvents() on the main thread,
 * so frames are driven via a Handler posted to the main Looper (~60 fps).
 */
class GameSurfaceView(context: Context, attrs: AttributeSet?) : SurfaceView(context, attrs),
    SurfaceHolder.Callback {

    private var gameRoot: String? = null
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

    override fun surfaceCreated(holder: SurfaceHolder) {
        val root = gameRoot ?: return
        try {
            org.libsdl.app.SDLActivity.bootstrap()
            Log.d("BS1", "SDL bootstrap ok")
        } catch (t: Throwable) {
            Log.e("BS1", "SDL bootstrap failed: ${t.message}")
        }
        org.libsdl.app.SDLActivity.setSurfaceOnly(holder.surface)
        initialized = NativeBridge.nativeInit(root, width, height) == 1
        Log.d("BS1", "nativeInit(root=$root, ${width}x$height) -> $initialized")
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
