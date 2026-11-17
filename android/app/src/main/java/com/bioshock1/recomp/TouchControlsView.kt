package com.bioshock1.recomp

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot

/**
 * Translucent dual-stick touch overlay.
 *
 * Left stick  -> axes 0/1 -> guest left thumbstick (movement / swimming).
 * Right stick -> axes 2/3 -> guest right thumbstick (look / camera).
 * Buttons     -> A/B/X/Y + START, indices matching nativeSetControls.
 *
 * Each stick is "floating": it activates wherever the finger first touches
 * within its region, so the player doesn't have to hit a tiny fixed circle.
 */
class TouchControlsView(context: Context) : View(context) {
    fun interface Listener {
        fun onControls(axes: FloatArray, buttons: ByteArray)
    }

    var listener: Listener? = null

    private val axes = FloatArray(8)
    private val buttons = ByteArray(32)
    private data class Pointer(val key: String, val startX: Float, val startY: Float)
    private val active = HashMap<Int, Pointer>()

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(210, 255, 255, 255)
        textAlign = Paint.Align.CENTER
        textSize = 32f
    }

    private data class Stick(val cx: Float, val cy: Float, val r: Float, val ax: Int, val ay: Int, val label: String)
    private data class Btn(val x: Float, val y: Float, val r: Float, val idx: Int, val label: String)

    // Full analog deflection after this many pixels of drag (smaller = more sensitive).
    private val deflectionPx = 60f

    private fun layout(w: Int, h: Int): Pair<List<Stick>, List<Btn>> {
        val sticks = listOf(
            Stick(w * 0.18f, h * 0.72f, 130f, 0, 1, "MOVE"),
            Stick(w * 0.66f, h * 0.72f, 130f, 2, 3, "LOOK"),
        )
        val btns = listOf(
            Btn(w * 0.88f, h * 0.30f, 42f, 0, "A"),
            Btn(w * 0.97f, h * 0.43f, 42f, 1, "B"),
            Btn(w * 0.79f, h * 0.43f, 42f, 2, "X"),
            Btn(w * 0.88f, h * 0.56f, 42f, 3, "Y"),
            Btn(w * 0.50f, h * 0.92f, 46f, 9, "START"),
        )
        return sticks to btns
    }

    override fun onDraw(canvas: Canvas) {
        val (sticks, btns) = layout(width, height)
        for (s in sticks) {
            paint.color = Color.argb(42, 200, 220, 255)
            canvas.drawCircle(s.cx, s.cy, s.r, paint)
            paint.color = Color.argb(70, 255, 255, 255)
            canvas.drawCircle(s.cx, s.cy, 8f, paint)
            paint.color = Color.argb(150, 255, 255, 255)
            canvas.drawText(s.label, s.cx, s.cy - s.r - 14f, textPaint)
            // Knob at the current analog position.
            val knobX = s.cx + axes[s.ax] * deflectionPx
            val knobY = s.cy - axes[s.ay] * deflectionPx
            paint.color = Color.argb(150, 120, 220, 160)
            canvas.drawCircle(knobX, knobY, 26f, paint)
        }
        for (b in btns) {
            paint.color = if (buttons[b.idx].toInt() != 0)
                Color.argb(170, 120, 220, 160)
            else
                Color.argb(70, 200, 220, 255)
            canvas.drawCircle(b.x, b.y, b.r, paint)
            canvas.drawText(b.label, b.x, b.y + 11f, textPaint)
        }
    }

    private fun classify(x: Float, y: Float): String? {
        val (sticks, btns) = layout(width, height)
        for (s in sticks) {
            if (hypot(x - s.cx, y - s.cy) < s.r) return if (s.ax == 0) "move" else "look"
        }
        for (b in btns) {
            if (hypot(x - b.x, y - b.y) < b.r) return "b${b.idx}"
        }
        return null
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val id = event.getPointerId(i)
                val key = classify(event.getX(i), event.getY(i))
                if (key != null) active[id] = Pointer(key, event.getX(i), event.getY(i))
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> {
                val id = event.getPointerId(event.actionIndex)
                active.remove(id)
                if (event.actionMasked == MotionEvent.ACTION_CANCEL) active.clear()
            }
        }

        axes.fill(0f)
        buttons.fill(0)
        for ((id, p) in active) {
            val idx = event.findPointerIndex(id)
            if (idx < 0) continue  // pointer lifted this frame
            val dx = event.getX(idx) - p.startX
            val dy = event.getY(idx) - p.startY
            when (p.key) {
                "move" -> {
                    axes[0] = (dx / deflectionPx).coerceIn(-1f, 1f)
                    axes[1] = (-dy / deflectionPx).coerceIn(-1f, 1f)
                }
                "look" -> {
                    axes[2] = (dx / deflectionPx).coerceIn(-1f, 1f)
                    axes[3] = (-dy / deflectionPx).coerceIn(-1f, 1f)
                }
                else -> if (p.key.startsWith("b")) {
                    p.key.removePrefix("b").toIntOrNull()?.let { buttons[it] = 1 }
                }
            }
        }

        listener?.onControls(axes.copyOf(), buttons.copyOf())
        invalidate()
        return true
    }
}
