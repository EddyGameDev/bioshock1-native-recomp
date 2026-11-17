package com.bioshock1.recomp

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot

/**
 * Minimal translucent touch overlay. Produces the same arrays consumed by
 * NativeBridge.nativeSetControls (8 axes, 32 buttons). Physical gamepads take
 * priority in a future revision; this overlay is the fallback for touch-only
 * devices and mirrors the layouts of the GBA/PS2 reference ports.
 */
class TouchControlsView(context: Context) : View(context) {
    fun interface Listener {
        fun onControls(axes: FloatArray, buttons: ByteArray)
    }

    var listener: Listener? = null

    private val axes = FloatArray(8)
    private val buttons = ByteArray(32)
    private val pointerStart = HashMap<Int, Pair<Float, Float>>()
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.argb(70, 200, 220, 255)
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(180, 255, 255, 255)
        textAlign = Paint.Align.CENTER
        textSize = 36f
    }

    private data class Circle(val x: Float, val y: Float, val r: Float, val button: Int, val label: String)

    private fun layout(w: Int, h: Int): Pair<Pair<Float, Float>, List<Circle>> {
        val left = Pair(w * 0.16f, h * 0.78f)
        val faces = listOf(
            Circle(w * 0.86f, h * 0.86f, 46f, 0, "A"),
            Circle(w * 0.78f, h * 0.76f, 46f, 1, "B"),
            Circle(w * 0.94f, h * 0.76f, 46f, 2, "X"),
            Circle(w * 0.86f, h * 0.66f, 46f, 3, "Y"),
            Circle(w * 0.50f, h * 0.90f, 40f, 9, "START"),
        )
        return left to faces
    }

    override fun onDraw(canvas: Canvas) {
        val (left, circles) = layout(width, height)
        paint.color = Color.argb(45, 200, 220, 255)
        canvas.drawCircle(left.first, left.second, 90f, paint)
        for (c in circles) {
            paint.color = if (buttons[c.button].toInt() != 0)
                Color.argb(140, 120, 220, 160)
            else Color.argb(70, 200, 220, 255)
            canvas.drawCircle(c.x, c.y, c.r, paint)
            canvas.drawText(c.label, c.x, c.y + 12f, textPaint)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val action = event.actionMasked
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            val i = event.actionIndex
            pointerStart[event.getPointerId(i)] = event.getX(i) to event.getY(i)
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ||
            action == MotionEvent.ACTION_CANCEL
        ) {
            val i = event.actionIndex
            val id = event.getPointerId(i)
            pointerStart.remove(id)
            if (action == MotionEvent.ACTION_POINTER_UP) {
                // Release every button associated with this pointer on move below.
                buttons.fill(0)
                axes[0] = 0f; axes[1] = 0f
            }
        }

        // Recompute from all active pointers.
        axes.fill(0f)
        if (action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL) {
            buttons.fill(0)
        }
        val (left, circles) = layout(width, height)
        for (id in pointerStart.keys) {
            val idx = event.findPointerIndex(id)
            if (idx < 0) continue
            val x = event.getX(idx)
            val y = event.getY(idx)
            val start = pointerStart[id] ?: continue
            val dx = x - start.first
            val dy = y - start.second
            if (hypot(dx, dy) < 90f && hypot(start.first - left.first, start.second - left.second) < 90f) {
                axes[0] = (dx / 70f).coerceIn(-1f, 1f)
                axes[1] = (-dy / 70f).coerceIn(-1f, 1f)
            }
            for (c in circles) {
                if (hypot(x - c.x, y - c.y) < c.r) buttons[c.button] = 1
            }
        }
        if (action == MotionEvent.ACTION_UP) {
            buttons.fill(0); axes.fill(0f)
        }
        listener?.onControls(axes.copyOf(), buttons.copyOf())
        invalidate()
        return true
    }
}
