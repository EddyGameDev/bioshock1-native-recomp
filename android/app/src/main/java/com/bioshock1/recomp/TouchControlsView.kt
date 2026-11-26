package com.bioshock1.recomp

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.MotionEvent
import android.view.View
import org.json.JSONArray
import org.json.JSONObject
import kotlin.math.hypot
import kotlin.math.min

/**
 * Full Xbox 360 on-screen touch controller tailored for BioShock 1.
 *
 * Supports an in-place editor (see [editMode]) that lets the player move, resize,
 * fade, rebind and preset every control without restarting the game.
 *
 * Guest index mapping (nativeSetControls):
 *   0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=LT, 7=RT, 8=BACK, 9=START,
 *   10=L3, 11=R3, 12=DPadUp, 13=DPadDown, 14=DPadLeft, 15=DPadRight
 * Axes: 0/1 = MOVE, 2/3 = LOOK.
 */
class TouchControlsView(context: Context, configJson: String? = null) : View(context) {
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

    // ------------------------------------------------------------------ config

    private data class Control(
        val id: String,
        val label: String,
        val sub: String,
        val guestIndex: Int,
        val tintRgb: Int,
    )

    private val buttonOrder = listOf(
        Control("A", "A", "", 0, 0x4ADE80),
        Control("B", "B", "", 1, 0xF87171),
        Control("X", "X", "", 2, 0x60A5FA),
        Control("Y", "Y", "", 3, 0xFACC15),
        Control("LB", "LB", "WHEEL", 4, 0x94A3B8),
        Control("RB", "RB", "WHEEL", 5, 0x94A3B8),
        Control("LT", "LT", "PLASMID", 6, 0x38BDF8),
        Control("RT", "RT", "FIRE", 7, 0xF59E0B),
        Control("BACK", "BACK", "", 8, 0x94A3B8),
        Control("START", "START", "", 9, 0x94A3B8),
        Control("L3", "L3", "", 10, 0x94A3B8),
        Control("R3", "R3", "", 11, 0x94A3B8),
    )

    private val rebindable = setOf(
        "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "BACK", "START", "L3", "R3"
    )

    private val defaultPositions: Map<String, Pair<Float, Float>> = mapOf(
        "move" to (0.135f to 0.76f),
        "look" to (0.725f to 0.76f),
        "dpad" to (0.105f to 0.44f),
        "A" to (0.895f to 0.645f),
        "B" to (0.943f to 0.540f),
        "X" to (0.847f to 0.540f),
        "Y" to (0.895f to 0.435f),
        "L3" to (0.255f to 0.860f),
        "R3" to (0.605f to 0.860f),
        "LT" to (0.063f to 0.171f),
        "LB" to (0.145f to 0.171f),
        "RB" to (0.855f to 0.171f),
        "RT" to (0.937f to 0.171f),
        "BACK" to (0.434f to 0.922f),
        "START" to (0.566f to 0.922f),
    )

    private val compactPositions = defaultPositions.mapValues { (_, p) ->
        val cx = 0.5f + (p.first - 0.5f) * 0.82f
        val cy = 0.5f + (p.second - 0.5f) * 0.82f
        cx to cy
    }

    private val leftHandedPositions = defaultPositions.mapValues { (_, p) ->
        val cx = if (p.first > 0.5f) 1.0f - p.first else p.first
        cx to p.second
    }

    private var presetName = "DEFAULT"
    private var positions = LinkedHashMap<String, Pair<Float, Float>>(defaultPositions)
    private var scale = 1.0f
    private var opacity = 1.0f
    private var bindings = LinkedHashMap<String, Int>().apply {
        for (c in buttonOrder) put(c.id, c.guestIndex)
    }
    private var configVersion = 0

    /** Runtime editor state; guest input is suppressed while true. */
    var editMode: Boolean = false
        set(value) {
            field = value
            if (!value) {
                selected = null
                dragging = false
            }
            invalidate()
        }

    private var selected: String? = null
    private var dragging = false
    private var dragDx = 0f
    private var dragDy = 0f

    init {
        configJson?.let { applyConfig(it) }
    }

    fun exportConfig(): JSONObject {
        val o = JSONObject()
        o.put("preset", presetName)
        o.put("scale", scale.toDouble())
        o.put("opacity", opacity.toDouble())
        val pos = JSONObject()
        for ((k, v) in positions) {
            val a = JSONArray()
            a.put(v.first.toDouble())
            a.put(v.second.toDouble())
            pos.put(k, a)
        }
        o.put("pos", pos)
        val bind = JSONObject()
        for ((k, v) in bindings) bind.put(k, v)
        o.put("bind", bind)
        return o
    }

    fun applyConfig(json: String) {
        try {
            val o = JSONObject(json)
            presetName = o.optString("preset", "CUSTOM")
            scale = o.optDouble("scale", 1.0).toFloat().coerceIn(0.6f, 1.6f)
            opacity = o.optDouble("opacity", 1.0).toFloat().coerceIn(0.25f, 1.0f)
            val pos = o.optJSONObject("pos")
            if (pos != null) {
                val loaded = LinkedHashMap<String, Pair<Float, Float>>()
                for ((k, v) in defaultPositions) {
                    val a = pos.optJSONArray(k)
                    if (a != null && a.length() >= 2) {
                        loaded[k] = a.getDouble(0).toFloat().coerceIn(0.02f, 0.98f) to
                            a.getDouble(1).toFloat().coerceIn(0.02f, 0.98f)
                    } else {
                        loaded[k] = v
                    }
                }
                positions = loaded
            }
            val bind = o.optJSONObject("bind")
            if (bind != null) {
                for (c in buttonOrder) {
                    bindings[c.id] = bind.optInt(c.id, c.guestIndex).coerceIn(0, 15)
                }
            }
            bumpConfig()
        } catch (_: Exception) {
            resetToDefault(false)
        }
    }

    private fun bumpConfig() {
        configVersion++
        cachedLayout = null
        invalidate()
    }

    private fun applyPreset(name: String) {
        presetName = name
        positions = LinkedHashMap<String, Pair<Float, Float>>(
            when (name) {
                "COMPACT" -> compactPositions
                "LEFT" -> leftHandedPositions
                "RIGHT" -> defaultPositions
                else -> defaultPositions
            }
        )
        scale = if (name == "COMPACT") 0.85f else 1.0f
        opacity = 1.0f
        for (c in buttonOrder) bindings[c.id] = c.guestIndex
        bumpConfig()
    }

    private fun resetToDefault(notify: Boolean = true) {
        if (notify) applyPreset("DEFAULT")
        else {
            presetName = "DEFAULT"
            positions = LinkedHashMap<String, Pair<Float, Float>>(defaultPositions)
            scale = 1.0f
            opacity = 1.0f
            for (c in buttonOrder) bindings[c.id] = c.guestIndex
        }
    }

    // ------------------------------------------------------------------ layout

    private data class Stick(
        val cx: Float, val cy: Float, val r: Float, val ax: Int, val ay: Int, val label: String,
    )
    private data class CircleBtn(
        val cx: Float, val cy: Float, val r: Float, val idx: Int, val label: String, val tintRgb: Int,
    )
    private data class PillBtn(
        val rect: RectF, val idx: Int, val label: String, val sub: String, val tintRgb: Int,
    )
    private data class DpadLayout(
        val cx: Float, val cy: Float, val arm: Float, val thick: Float,
        val up: RectF, val down: RectF, val left: RectF, val right: RectF, val hitRect: RectF,
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
    private var cachedVersion = -1
    private var cachedLayout: Layout? = null

    private fun px(x: Float, w: Int): Float = x * w
    private fun py(y: Float, h: Int): Float = y * h

    private fun getLayout(w: Int, h: Int): Layout {
        val existing = cachedLayout
        if (existing != null && w == cachedW && h == cachedH && configVersion == cachedVersion) {
            return existing
        }
        val u = min(w, h).toFloat().coerceAtLeast(360f) * scale
        val stickR = u * 0.125f
        val deflection = stickR * 0.62f

        val movePos = positions.getValue("move")
        val lookPos = positions.getValue("look")
        val sticks = listOf(
            Stick(px(movePos.first, w), py(movePos.second, h), stickR, 0, 1, "MOVE"),
            Stick(px(lookPos.first, w), py(lookPos.second, h), stickR, 2, 3, "LOOK"),
        )

        val dpos = positions.getValue("dpad")
        val dpadCx = px(dpos.first, w)
        val dpadCy = py(dpos.second, h)
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
            hitRect = RectF(dpadCx - arm * 1.18f, dpadCy - arm * 1.18f,
                dpadCx + arm * 1.18f, dpadCy + arm * 1.18f),
        )

        val faceR = u * 0.048f
        val clickR = u * 0.036f
        val circles = ArrayList<CircleBtn>(6)
        for (id in listOf("A", "B", "X", "Y", "L3", "R3")) {
            val p = positions.getValue(id)
            val c = buttonOrder.first { it.id == id }
            circles.add(
                CircleBtn(
                    px(p.first, w), py(p.second, h),
                    if (id == "L3" || id == "R3") clickR else faceR,
                    bindings[id] ?: c.guestIndex, c.label, c.tintRgb,
                )
            )
        }

        val pillW = u * 0.145f
        val pillH = u * 0.072f
        val pills = ArrayList<PillBtn>(6)
        for (id in listOf("LT", "LB", "RB", "RT", "BACK", "START")) {
            val p = positions.getValue(id)
            val c = buttonOrder.first { it.id == id }
            val cx = px(p.first, w)
            val cy = py(p.second, h)
            pills.add(
                PillBtn(
                    RectF(cx - pillW * 0.5f, cy - pillH * 0.5f, cx + pillW * 0.5f, cy + pillH * 0.5f),
                    bindings[id] ?: c.guestIndex, c.label, c.sub, c.tintRgb,
                )
            )
        }

        val built = Layout(sticks, circles, pills, dpad, deflection)
        cachedW = w
        cachedH = h
        cachedVersion = configVersion
        cachedLayout = built
        return built
    }

    // ------------------------------------------------------------------ drawing

    private fun alpha(a: Int): Int = (a * opacity).toInt().coerceIn(0, 255)
    private fun rgbWithAlpha(alpha: Int, rgb: Int): Int =
        Color.argb(alpha, (rgb shr 16) and 0xFF, (rgb shr 8) and 0xFF, rgb and 0xFF)

    private fun highlight(cx: Float, cy: Float, r: Float) {
        strokePaint.color = Color.argb(255, 255, 255, 180)
        strokePaint.strokeWidth = 4f
        canvasForHighlight?.drawCircle(cx, cy, r + 8f, strokePaint)
        strokePaint.strokeWidth = 2.5f
    }

    private var canvasForHighlight: Canvas? = null

    override fun onDraw(canvas: Canvas) {
        if (width <= 0 || height <= 0) return
        canvasForHighlight = canvas
        val lay = getLayout(width, height)
        val u = min(width, height).toFloat().coerceAtLeast(360f) * scale

        for (s in lay.sticks) {
            fillPaint.color = Color.argb(alpha(38), 180, 210, 245)
            canvas.drawCircle(s.cx, s.cy, s.r, fillPaint)
            strokePaint.color = Color.argb(alpha(95), 200, 225, 255)
            canvas.drawCircle(s.cx, s.cy, s.r, strokePaint)
            fillPaint.color = Color.argb(alpha(75), 255, 255, 255)
            canvas.drawCircle(s.cx, s.cy, 5f, fillPaint)
            val knobX = s.cx + axes[s.ax] * lay.deflectionPx
            val knobY = s.cy - axes[s.ay] * lay.deflectionPx
            val activeStick = axes[s.ax] != 0f || axes[s.ay] != 0f
            fillPaint.color = if (activeStick) Color.argb(alpha(185), 80, 220, 165)
                              else Color.argb(alpha(125), 160, 200, 230)
            canvas.drawCircle(knobX, knobY, s.r * 0.34f, fillPaint)
            strokePaint.color = Color.argb(alpha(175), 235, 245, 255)
            canvas.drawCircle(knobX, knobY, s.r * 0.34f, strokePaint)
            subLabelPaint.textSize = u * 0.024f
            canvas.drawText(s.label, s.cx, s.cy - s.r - u * 0.012f, subLabelPaint)
            if (editMode && selected == s.label) highlight(s.cx, s.cy, s.r)
        }

        val d = lay.dpad
        val corner = u * 0.012f
        val dpadParts = listOf(
            Triple(d.up, 12, "▲"), Triple(d.down, 13, "▼"),
            Triple(d.left, 14, "◄"), Triple(d.right, 15, "►"),
        )
        labelPaint.textSize = u * 0.028f
        for ((rect, idx, glyph) in dpadParts) {
            val pressed = buttons[idx].toInt() != 0
            fillPaint.color = if (pressed) Color.argb(alpha(175), 96, 215, 170)
                              else Color.argb(alpha(58), 180, 205, 235)
            canvas.drawRoundRect(rect, corner, corner, fillPaint)
            strokePaint.color = if (pressed) Color.argb(alpha(220), 230, 255, 240)
                                else Color.argb(alpha(105), 200, 220, 245)
            canvas.drawRoundRect(rect, corner, corner, strokePaint)
            canvas.drawText(glyph, rect.centerX(), rect.centerY() + labelPaint.textSize * 0.34f, labelPaint)
        }
        if (editMode && selected == "dpad") highlight(d.cx, d.cy, d.arm)

        for (b in lay.circles) {
            val pressed = buttons[b.idx].toInt() != 0
            fillPaint.color = rgbWithAlpha(if (pressed) alpha(190) else alpha(68), b.tintRgb)
            canvas.drawCircle(b.cx, b.cy, b.r, fillPaint)
            strokePaint.color = rgbWithAlpha(if (pressed) alpha(245) else alpha(155), b.tintRgb)
            canvas.drawCircle(b.cx, b.cy, b.r, strokePaint)
            labelPaint.textSize = b.r * 0.72f
            canvas.drawText(b.label, b.cx, b.cy + labelPaint.textSize * 0.35f, labelPaint)
            if (editMode && selected == b.label) highlight(b.cx, b.cy, b.r)
        }

        for (p in lay.pills) {
            val pressed = buttons[p.idx].toInt() != 0
            val rad = p.rect.height() * 0.32f
            fillPaint.color = rgbWithAlpha(if (pressed) alpha(190) else alpha(65), p.tintRgb)
            canvas.drawRoundRect(p.rect, rad, rad, fillPaint)
            strokePaint.color = rgbWithAlpha(if (pressed) alpha(245) else alpha(145), p.tintRgb)
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
            if (editMode && selected == p.label) {
                strokePaint.color = Color.argb(255, 255, 255, 180)
                strokePaint.strokeWidth = 4f
                canvas.drawRoundRect(p.rect, rad, rad, strokePaint)
                strokePaint.strokeWidth = 2.5f
            }
        }

        if (editMode) drawToolbar(canvas)
        canvasForHighlight = null
    }

    // ------------------------------------------------------------------ editor UI

    private val toolbarActions = listOf("SEL", "S-", "S+", "O-", "O+", "BIND", "PRESET", "RESET", "DONE")

    private fun toolbarRects(): List<RectF> {
        val bw = width * 0.038f
        val bh = min(height * 0.052f, 64f)
        val gap = width * 0.004f
        val total = toolbarActions.size * bw + (toolbarActions.size - 1) * gap
        val x0 = (width - total) * 0.5f
        val y = height * 0.085f
        return toolbarActions.indices.map { i ->
            RectF(x0 + i * (bw + gap), y, x0 + i * (bw + gap) + bw, y + bh)
        }
    }

    private fun drawToolbar(canvas: Canvas) {
        val rects = toolbarRects()
        for (i in rects.indices) {
            val r = rects[i]
            fillPaint.color = Color.argb(215, 16, 28, 44)
            canvas.drawRoundRect(r, 12f, 12f, fillPaint)
            strokePaint.color = Color.argb(200, 180, 210, 255)
            canvas.drawRoundRect(r, 12f, 12f, strokePaint)
            labelPaint.textSize = r.height() * 0.36f
            canvas.drawText(toolbarActions[i], r.centerX(), r.centerY() + labelPaint.textSize * 0.34f, labelPaint)
        }
        labelPaint.textSize = min(width, height) * 0.022f
        val msg = when (selected) {
            null -> "TAP A CONTROL TO SELECT  •  DRAG TO MOVE"
            else -> "SELECTED: $selected  •  BIND=${bindings[selected] ?: "-"}"
        }
        canvas.drawText(msg, width * 0.5f, rects[0].bottom + labelPaint.textSize * 1.4f, labelPaint)
    }

    private fun classify(x: Float, y: Float): String? {
        val lay = getLayout(width, height)
        for (p in lay.pills) {
            if (x >= p.rect.left - 14f && x <= p.rect.right + 14f &&
                y >= p.rect.top - 14f && y <= p.rect.bottom + 14f
            ) return p.label
        }
        for (c in lay.circles) {
            if (hypot(x - c.cx, y - c.cy) <= c.r * 1.28f) return c.label
        }
        if (lay.dpad.hitRect.contains(x, y)) return "dpad"
        for (s in lay.sticks) {
            if (hypot(x - s.cx, y - s.cy) <= s.r * 1.25f) {
                return if (s.ax == 0) "MOVE" else "LOOK"
            }
        }
        return null
    }

    private fun applyDpad(x: Float, y: Float, d: DpadLayout) {
        val dx = x - d.cx
        val dy = y - d.cy
        val dead = d.thick * 0.28f
        if (hypot(dx, dy) < dead) return
        if (dy < -dead) buttons[12] = 1
        if (dy > dead) buttons[13] = 1
        if (dx < -dead) buttons[14] = 1
        if (dx > dead) buttons[15] = 1
    }

    private fun toolbarAction(action: String) {
        when (action) {
            "DONE" -> editMode = false
            "RESET" -> resetToDefault()
            "PRESET" -> {
                val names = listOf("DEFAULT", "COMPACT", "LEFT", "RIGHT")
                val i = names.indexOf(presetName).coerceAtLeast(0)
                applyPreset(names[(i + 1) % names.size])
            }
            "S-" -> { scale = (scale - 0.1f).coerceIn(0.6f, 1.6f); bumpConfig() }
            "S+" -> { scale = (scale + 0.1f).coerceIn(0.6f, 1.6f); bumpConfig() }
            "O-" -> { opacity = (opacity - 0.1f).coerceIn(0.25f, 1.0f); bumpConfig() }
            "O+" -> { opacity = (opacity + 0.1f).coerceIn(0.25f, 1.0f); bumpConfig() }
            "BIND" -> {
                val id = selected
                if (id != null && id in rebindable) {
                    val cur = bindings[id] ?: 0
                    bindings[id] = (cur + 1) % 16
                    bumpConfig()
                }
            }
            "SEL" -> selected = null
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (width <= 0 || height <= 0) return false

        if (editMode) {
            val x = event.getX(0)
            val y = event.getY(0)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                    val idx = toolbarRects().indexOfFirst { it.contains(x, y) }
                    if (idx >= 0) {
                        toolbarAction(toolbarActions[idx])
                        invalidate()
                        return true
                    }
                    val hit = classify(x, y)
                    if (hit != null) {
                        selected = hit
                        val p = positions[hit]
                        if (p != null) {
                            dragDx = p.first - x / width
                            dragDy = p.second - y / height
                            dragging = true
                        }
                        invalidate()
                    }
                }
                MotionEvent.ACTION_MOVE -> {
                    if (dragging && selected != null) {
                        val nx = (x / width + dragDx).coerceIn(0.02f, 0.98f)
                        val ny = (y / height + dragDy).coerceIn(0.02f, 0.98f)
                        positions[selected!!] = nx to ny
                        presetName = "CUSTOM"
                        bumpConfig()
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> dragging = false
            }
            return true
        }

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
                } else if (active.isEmpty()) {
                    return false  // pass through to HUD pills
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
                "MOVE", "LOOK" -> {
                    val dist = hypot(dx, dy)
                    val clamped = if (dist > lay.deflectionPx && dist > 0f) lay.deflectionPx / dist else 1f
                    val nx = ((dx * clamped) / lay.deflectionPx).coerceIn(-1f, 1f)
                    val ny = ((-dy * clamped) / lay.deflectionPx).coerceIn(-1f, 1f)
                    if (p.key == "MOVE") {
                        axes[0] = nx; axes[1] = ny
                    } else {
                        axes[2] = nx; axes[3] = ny
                    }
                }
                "dpad" -> applyDpad(curX, curY, lay.dpad)
                else -> bindings[p.key]?.let { buttons[it] = 1 }
            }
        }

        listener?.onControls(axes.copyOf(), buttons.copyOf())
        invalidate()
        return true
    }
}
