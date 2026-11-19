package com.bioshock1.recomp

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import kotlin.math.hypot
import kotlin.math.min

/**
 * Full Xbox 360 on-screen touch controller tailored for BioShock 1.
 *
 * Provides every input needed to play the complete game from start to finish:
 *  - Left Stick (MOVE) + Right Stick (LOOK) with floating origin & circular clamping
 *  - Shoulder & Trigger pills: LT (Plasmid), LB (Plasmid Wheel), RT (Fire), RB (Weapon Wheel)
 *  - Color-coded Xbox 360 face diamond: Y (Jump), X (Reload/Hack), B (Cancel/Switch), A (Use)
 *  - 4-way D-Pad (slide-friendly): Up (First Aid), Down (EVE/Radio), Left/Right (Ammo)
 *  - Stick clicks: L3 (Crouch), R3 (Zoom)
 *  - System pills: BACK (Map/Diaries), START (Pause)
 *
 * Button index mapping matches Java_com_bioshock1_recomp_NativeBridge_nativeSetControls:
 *   0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=LT, 7=RT, 8=BACK, 9=START,
 *   10=L3, 11=R3, 12=DPadUp, 13=DPadDown, 14=DPadLeft, 15=DPadRight
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

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2.5f
    }
    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(230, 240, 246, 255)
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
    }
    private val subLabelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(175, 210, 224, 240)
        textAlign = Paint.Align.CENTER
    }

    private data class Stick(
        val cx: Float,
        val cy: Float,
        val r: Float,
        val ax: Int,
        val ay: Int,
        val label: String,
    )

    private data class CircleBtn(
        val cx: Float,
        val cy: Float,
        val r: Float,
        val idx: Int,
        val label: String,
        val tintRgb: Int,
    )

    private data class PillBtn(
        val rect: RectF,
        val idx: Int,
        val label: String,
        val sub: String,
        val tintRgb: Int,
    )

    private data class DpadLayout(
        val cx: Float,
        val cy: Float,
        val arm: Float,
        val thick: Float,
        val up: RectF,
        val down: RectF,
        val left: RectF,
        val right: RectF,
        val hitRect: RectF,
    )

    private data class Layout(
        val sticks: List<Stick>,
        val circles: List<CircleBtn>,
        val pills: List<PillBtn>,
        val dpad: DpadLayout,
        val deflectionPx: Float,
    )

    private var cachedW = 0
    private var cachedH = 0
    private var cachedLayout: Layout? = null

    private fun getLayout(w: Int, h: Int): Layout {
        val existing = cachedLayout
        if (existing != null && w == cachedW && h == cachedH) return existing

        val u = min(w, h).toFloat().coerceAtLeast(360f)
        val stickR = u * 0.125f
        val deflection = stickR * 0.62f

        // Dual analog sticks: comfortable lower-left and lower-right thumb zones.
        val moveStick = Stick(w * 0.135f, h * 0.76f, stickR, 0, 1, "MOVE")
        val lookStick = Stick(w * 0.725f, h * 0.76f, stickR, 2, 3, "LOOK")

        // 4-way D-Pad: left side above the MOVE stick (Medkit / EVE / Ammo).
        val dpadCx = w * 0.105f
        val dpadCy = h * 0.44f
        val arm = u * 0.095f
        val thick = u * 0.056f
        val halfT = thick * 0.5f
        val dpad = DpadLayout(
            cx = dpadCx,
            cy = dpadCy,
            arm = arm,
            thick = thick,
            up = RectF(dpadCx - halfT, dpadCy - arm, dpadCx + halfT, dpadCy - halfT * 0.35f),
            down = RectF(dpadCx - halfT, dpadCy + halfT * 0.35f, dpadCx + halfT, dpadCy + arm),
            left = RectF(dpadCx - arm, dpadCy - halfT, dpadCx - halfT * 0.35f, dpadCy + halfT),
            right = RectF(dpadCx + halfT * 0.35f, dpadCy - halfT, dpadCx + arm, dpadCy + halfT),
            hitRect = RectF(dpadCx - arm * 1.18f, dpadCy - arm * 1.18f, dpadCx + arm * 1.18f, dpadCy + arm * 1.18f),
        )

        // Color-coded Xbox 360 face diamond on the right side (Y top, X left, B right, A bottom).
        val faceCx = w * 0.895f
        val faceCy = h * 0.54f
        val faceSpread = u * 0.105f
        val faceR = u * 0.048f
        val stickClickR = u * 0.036f

        val circles = listOf(
            CircleBtn(faceCx, faceCy + faceSpread, faceR, 0, "A", 0x4ADE80), // Green: Use / Search
            CircleBtn(faceCx + faceSpread, faceCy, faceR, 1, "B", 0xF87171), // Red: Cancel / Switch
            CircleBtn(faceCx - faceSpread, faceCy, faceR, 2, "X", 0x60A5FA), // Blue: Reload / Hack
            CircleBtn(faceCx, faceCy - faceSpread, faceR, 3, "Y", 0xFACC15), // Yellow: Jump
            // Stick clicks tucked inboard of each stick.
            CircleBtn(w * 0.255f, h * 0.86f, stickClickR, 10, "L3", 0x94A3B8), // Crouch
            CircleBtn(w * 0.605f, h * 0.86f, stickClickR, 11, "R3", 0x94A3B8), // Zoom
        )

        // Shoulder triggers & bumpers (upper-left below HUD bar, upper-right mirrored).
        val pillW = u * 0.145f
        val pillH = u * 0.072f
        val leftStartX = w * 0.03f
        val rightEndX = w * 0.97f
        val topRowY = h * 0.135f
        val gap = u * 0.018f

        // System pills (BACK / START) centered along the bottom edge.
        val sysW = u * 0.125f
        val sysH = u * 0.056f
        val sysY = h - sysH - u * 0.022f
        val centerX = w * 0.50f

        val pills = listOf(
            // Left triggers: LT (Plasmid) + LB (Plasmid Wheel)
            PillBtn(
                RectF(leftStartX, topRowY, leftStartX + pillW, topRowY + pillH),
                6, "LT", "PLASMID", 0x38BDF8,
            ),
            PillBtn(
                RectF(leftStartX + pillW + gap, topRowY, leftStartX + pillW * 2f + gap, topRowY + pillH),
                4, "LB", "WHEEL", 0x94A3B8,
            ),
            // Right triggers: RB (Weapon Wheel) + RT (Fire Weapon)
            PillBtn(
                RectF(rightEndX - pillW * 2f - gap, topRowY, rightEndX - pillW - gap, topRowY + pillH),
                5, "RB", "WHEEL", 0x94A3B8,
            ),
            PillBtn(
                RectF(rightEndX - pillW, topRowY, rightEndX, topRowY + pillH),
                7, "RT", "FIRE", 0xF59E0B,
            ),
            // Bottom-center system pills
            PillBtn(
                RectF(centerX - sysW - gap * 0.8f, sysY, centerX - gap * 0.8f, sysY + sysH),
                8, "BACK", "", 0x94A3B8,
            ),
            PillBtn(
                RectF(centerX + gap * 0.8f, sysY, centerX + sysW + gap * 0.8f, sysY + sysH),
                9, "START", "", 0x94A3B8,
            ),
        )

        val built = Layout(
            sticks = listOf(moveStick, lookStick),
            circles = circles,
            pills = pills,
            dpad = dpad,
            deflectionPx = deflection,
        )
        cachedW = w
        cachedH = h
        cachedLayout = built
        return built
    }

    private fun rgbWithAlpha(alpha: Int, rgb: Int): Int =
        Color.argb(alpha, (rgb shr 16) and 0xFF, (rgb shr 8) and 0xFF, rgb and 0xFF)

    override fun onDraw(canvas: Canvas) {
        if (width <= 0 || height <= 0) return
        val lay = getLayout(width, height)
        val u = min(width, height).toFloat().coerceAtLeast(360f)

        // 1) Dual Analog Sticks
        for (s in lay.sticks) {
            fillPaint.color = Color.argb(38, 180, 210, 245)
            canvas.drawCircle(s.cx, s.cy, s.r, fillPaint)
            strokePaint.color = Color.argb(95, 200, 225, 255)
            canvas.drawCircle(s.cx, s.cy, s.r, strokePaint)

            // Center dot
            fillPaint.color = Color.argb(75, 255, 255, 255)
            canvas.drawCircle(s.cx, s.cy, 5f, fillPaint)

            // Knob
            val knobX = s.cx + axes[s.ax] * lay.deflectionPx
            val knobY = s.cy - axes[s.ay] * lay.deflectionPx
            val stickActive = (axes[s.ax] != 0f || axes[s.ay] != 0f)
            fillPaint.color = if (stickActive) Color.argb(185, 80, 220, 165) else Color.argb(125, 160, 200, 230)
            canvas.drawCircle(knobX, knobY, s.r * 0.34f, fillPaint)
            strokePaint.color = Color.argb(175, 235, 245, 255)
            canvas.drawCircle(knobX, knobY, s.r * 0.34f, strokePaint)

            subLabelPaint.textSize = u * 0.024f
            canvas.drawText(s.label, s.cx, s.cy - s.r - u * 0.012f, subLabelPaint)
        }

        // 2) 4-Way D-Pad
        val d = lay.dpad
        val corner = u * 0.012f
        val dpadParts = listOf(
            Triple(d.up, 12, "▲"),
            Triple(d.down, 13, "▼"),
            Triple(d.left, 14, "◄"),
            Triple(d.right, 15, "►"),
        )
        labelPaint.textSize = u * 0.028f
        for ((rect, idx, glyph) in dpadParts) {
            val pressed = buttons[idx].toInt() != 0
            fillPaint.color = if (pressed) Color.argb(175, 96, 215, 170) else Color.argb(58, 180, 205, 235)
            canvas.drawRoundRect(rect, corner, corner, fillPaint)
            strokePaint.color = if (pressed) Color.argb(220, 230, 255, 240) else Color.argb(105, 200, 220, 245)
            canvas.drawRoundRect(rect, corner, corner, strokePaint)
            canvas.drawText(glyph, rect.centerX(), rect.centerY() + labelPaint.textSize * 0.34f, labelPaint)
        }

        // 3) Face Buttons & Stick Clicks (Circles)
        for (b in lay.circles) {
            val pressed = buttons[b.idx].toInt() != 0
            fillPaint.color = rgbWithAlpha(if (pressed) 190 else 68, b.tintRgb)
            canvas.drawCircle(b.cx, b.cy, b.r, fillPaint)
            strokePaint.color = rgbWithAlpha(if (pressed) 245 else 155, b.tintRgb)
            canvas.drawCircle(b.cx, b.cy, b.r, strokePaint)

            labelPaint.textSize = b.r * 0.72f
            canvas.drawText(b.label, b.cx, b.cy + labelPaint.textSize * 0.35f, labelPaint)
        }

        // 4) Shoulder Triggers / Bumpers / BACK / START (Pills)
        for (p in lay.pills) {
            val pressed = buttons[p.idx].toInt() != 0
            val rad = p.rect.height() * 0.32f
            fillPaint.color = rgbWithAlpha(if (pressed) 190 else 65, p.tintRgb)
            canvas.drawRoundRect(p.rect, rad, rad, fillPaint)
            strokePaint.color = rgbWithAlpha(if (pressed) 245 else 145, p.tintRgb)
            canvas.drawRoundRect(p.rect, rad, rad, strokePaint)

            if (p.sub.isNotEmpty()) {
                labelPaint.textSize = p.rect.height() * 0.40f
                subLabelPaint.textSize = p.rect.height() * 0.24f
                canvas.drawText(p.label, p.rect.centerX(), p.rect.centerY() - p.rect.height() * 0.02f, labelPaint)
                canvas.drawText(p.sub, p.rect.centerX(), p.rect.centerY() + p.rect.height() * 0.32f, subLabelPaint)
            } else {
                labelPaint.textSize = p.rect.height() * 0.42f
                canvas.drawText(p.label, p.rect.centerX(), p.rect.centerY() + labelPaint.textSize * 0.35f, labelPaint)
            }
        }
    }

    private fun classify(x: Float, y: Float): String? {
        val lay = getLayout(width, height)
        for (p in lay.pills) {
            if (p.rect.contains(x, y)) return "b${p.idx}"
        }
        for (c in lay.circles) {
            if (hypot(x - c.cx, y - c.cy) <= c.r * 1.15f) return "b${c.idx}"
        }
        if (lay.dpad.hitRect.contains(x, y)) return "dpad"
        for (s in lay.sticks) {
            if (hypot(x - s.cx, y - s.cy) <= s.r * 1.25f) {
                return if (s.ax == 0) "move" else "look"
            }
        }
        return null
    }

    private fun applyDpad(x: Float, y: Float, d: DpadLayout) {
        val dx = x - d.cx
        val dy = y - d.cy
        val dead = d.thick * 0.28f
        if (hypot(dx, dy) < dead) return
        if (dy < -dead) buttons[12] = 1 // Up
        if (dy > dead) buttons[13] = 1  // Down
        if (dx < -dead) buttons[14] = 1 // Left
        if (dx > dead) buttons[15] = 1  // Right
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (width <= 0 || height <= 0) return false
        val lay = getLayout(width, height)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val i = event.actionIndex
                val id = event.getPointerId(i)
                val x = event.getX(i)
                val y = event.getY(i)
                val key = classify(x, y)
                if (key != null) {
                    active[id] = Pointer(key, x, y)
                } else {
                    // Let touches outside controls pass through (e.g. HUD pills).
                    if (active.isEmpty()) return false
                }
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
            if (idx < 0) continue
            val curX = event.getX(idx)
            val curY = event.getY(idx)
            val dx = curX - p.startX
            val dy = curY - p.startY
            when (p.key) {
                "move", "look" -> {
                    val dist = hypot(dx, dy)
                    val clamped = if (dist > lay.deflectionPx && dist > 0f) lay.deflectionPx / dist else 1f
                    val nx = ((dx * clamped) / lay.deflectionPx).coerceIn(-1f, 1f)
                    val ny = ((-dy * clamped) / lay.deflectionPx).coerceIn(-1f, 1f)
                    if (p.key == "move") {
                        axes[0] = nx
                        axes[1] = ny
                    } else {
                        axes[2] = nx
                        axes[3] = ny
                    }
                }
                "dpad" -> applyDpad(curX, curY, lay.dpad)
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
