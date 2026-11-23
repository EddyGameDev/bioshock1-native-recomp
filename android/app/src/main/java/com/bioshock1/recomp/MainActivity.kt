package com.bioshock1.recomp

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.sign

class MainActivity : Activity() {
    private lateinit var launcherPanel: View
    private lateinit var status: TextView
    private lateinit var importButton: Button
    private lateinit var startButton: Button
    private lateinit var driverButton: Button
    private lateinit var scaleButton: Button
    private lateinit var upscaleButton: Button
    private lateinit var fxaaButton: Button
    private lateinit var perfButton: Button
    private lateinit var exportLogsButton: Button
    private lateinit var container: FrameLayout
    private var surface: GameSurfaceView? = null
    private var touchOverlay: TouchControlsView? = null
    private var fpsView: TextView? = null
    private var liveResBtn: TextView? = null
    private var liveFilterBtn: TextView? = null
    private var liveTouchBtn: TextView? = null

    private val prefs by lazy { getSharedPreferences("bs1_render_prefs", MODE_PRIVATE) }
    private val resPresets = listOf("360p", "540p", "720p", "900p", "1080p", "1440p")
    private var driverMode = "custom"
    private var internalRes = "720p"
    private var vkScale = 1
    private var upscaleMode = "lanczos"
    private var fxaaEnabled = true
    private var perfModeEnabled = true

    private val hudHandler = Handler(Looper.getMainLooper())
    private var lastPresentCount = 0L
    private var lastFpsTimeNs = 0L

    // Separate touch and hardware-gamepad states merged before sending to nativeSetControls().
    private val touchAxes = FloatArray(8)
    private val touchButtons = ByteArray(32)
    private val padAxes = FloatArray(8)
    private val padButtons = ByteArray(32)
    private val mergedAxes = FloatArray(8)
    private val mergedButtons = ByteArray(32)
    private var padDpadKeyMask = 0
    private var padDpadHatMask = 0
    private var padLtKey = false
    private var padRtKey = false
    private var padLtAxis = false
    private var padRtAxis = false
    private val fpsRunnable = object : Runnable {
        override fun run() {
            val now = System.nanoTime()
            val count = NativeBridge.nativeGetPresentCount()
            if (lastFpsTimeNs != 0L && now > lastFpsTimeNs) {
                val dtSec = (now - lastFpsTimeNs) / 1e9
                val fps = ((count - lastPresentCount) / dtSec).toInt().coerceAtLeast(0)
                fpsView?.text = "${fps} FPS"
            }
            lastPresentCount = count
            lastFpsTimeNs = now
            hudHandler.postDelayed(this, 1000)
        }
    }

    private val gameDir: File by lazy { File(filesDir, "game").apply { mkdirs() } }

    fun messageboxShowMessageBox(
        flags: Int, title: String?, message: String?,
        buttonFlags: IntArray?, buttonIds: IntArray?,
        buttonTexts: Array<String>?, colors: IntArray?
    ): Int {
        android.util.Log.e("BS1-MSGBOX", "$title: $message")
        return buttonIds?.firstOrNull() ?: 0
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        org.libsdl.app.SDLActivity.setContext(this)
        setContentView(R.layout.activity_main)
        applyImmersiveFullscreen()

        launcherPanel = findViewById(R.id.launcherPanel)
        status = findViewById(R.id.status)
        importButton = findViewById(R.id.importButton)
        startButton = findViewById(R.id.startButton)
        driverButton = findViewById(R.id.driverButton)
        scaleButton = findViewById(R.id.scaleButton)
        upscaleButton = findViewById(R.id.upscaleButton)
        fxaaButton = findViewById(R.id.fxaaButton)
        perfButton = findViewById(R.id.perfButton)
        exportLogsButton = findViewById(R.id.exportLogsButton)
        container = findViewById(R.id.surfaceContainer)

        driverMode = prefs.getString("driver_mode", "custom") ?: "custom"
        internalRes = prefs.getString("internal_res", "540p") ?: "540p"
        if (internalRes !in resPresets) internalRes = "540p"
        vkScale = prefs.getInt("res_scale", 1).coerceIn(1, 2)
        upscaleMode = prefs.getString("upscale_mode", "lanczos") ?: "lanczos"
        fxaaEnabled = prefs.getBoolean("fxaa", true)
        perfModeEnabled = prefs.getBoolean("perf_mode", true)

        // Optional ADB test overrides via Intent extras.
        intent?.getStringExtra("driver")?.let { if (it == "custom" || it == "vulkan") driverMode = it }
        intent?.getStringExtra("res")?.let { if (it in resPresets) internalRes = it }

        driverButton.setOnClickListener {
            driverMode = if (driverMode == "custom") "vulkan" else "custom"
            prefs.edit().putString("driver_mode", driverMode).apply()
            updateSettingButtons()
        }
        scaleButton.setOnClickListener {
            if (driverMode == "custom") {
                val idx = resPresets.indexOf(internalRes).coerceAtLeast(0)
                internalRes = resPresets[(idx + 1) % resPresets.size]
                prefs.edit().putString("internal_res", internalRes).apply()
            } else {
                vkScale = if (vkScale == 1) 2 else 1
                prefs.edit().putInt("res_scale", vkScale).apply()
            }
            updateSettingButtons()
        }
        upscaleButton.setOnClickListener {
            upscaleMode = if (upscaleMode == "lanczos") "bilinear" else "lanczos"
            prefs.edit().putString("upscale_mode", upscaleMode).apply()
            updateSettingButtons()
        }
        fxaaButton.setOnClickListener {
            fxaaEnabled = !fxaaEnabled
            prefs.edit().putBoolean("fxaa", fxaaEnabled).apply()
            updateSettingButtons()
        }
        perfButton.setOnClickListener {
            perfModeEnabled = !perfModeEnabled
            prefs.edit().putBoolean("perf_mode", perfModeEnabled).apply()
            updateSettingButtons()
        }

        importButton.setOnClickListener { openDocument() }
        startButton.setOnClickListener { startGame() }
        exportLogsButton.setOnClickListener { exportLogs() }
        updateSettingButtons()
        updateStatus()

        if (intent?.getBooleanExtra("autostart", false) == true) {
            container.post { startGame() }
        }
    }

    /** Capture the app's logcat and share it as a text file (for Discord bug reports). */
    private fun exportLogs() {
        exportLogsButton.isEnabled = false
        exportLogsButton.text = "EXPORTING..."
        CoroutineScope(Dispatchers.IO).launch {
            val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
            val file = File(getExternalFilesDir(null), "bioshock1_logs_$stamp.txt")
            try {
                val version = try {
                    packageManager.getPackageInfo(packageName, 0).versionName
                } catch (_: Exception) {
                    "unknown"
                }
                file.writeText(
                    "BioShock 1 Native ReComp - diagnostic logs\n" +
                        "Version: $version\n" +
                        "Device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})\n" +
                        "Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n" +
                        "GPU: ${Build.SUPPORTED_ABIS.joinToString(", ")}\n" +
                        "Time: $stamp\n\n"
                )
                val proc = Runtime.getRuntime().exec(
                    arrayOf(
                        "logcat", "-d", "-v", "threadtime", "-s",
                        "BS1-GLES3:I", "BS1-JNI:I", "BS1-VDSWAP:I", "BS1-MSGBOX:E", "BS1-UI:I",
                        "AndroidRuntime:E", "DEBUG:E", "libc:E", "crash_dump64:E", "tombstoned:I"
                    )
                )
                proc.inputStream.bufferedReader(Charsets.UTF_8).useLines { lines ->
                    file.appendText(lines.joinToString("\n"))
                }
                proc.errorStream.bufferedReader(Charsets.UTF_8).useLines { lines ->
                    val err = lines.joinToString("\n").trim()
                    if (err.isNotEmpty()) file.appendText("\n[logcat stderr]\n$err\n")
                }
                proc.waitFor()
                val runtimeLog = File(getExternalFilesDir(null), "bs1-runtime.log")
                if (runtimeLog.exists()) {
                    val contents = runtimeLog.readText().trim()
                    if (contents.isNotEmpty()) {
                        file.appendText("\n=== Native renderer log ===\n$contents\n")
                    }
                }
            } catch (e: Exception) {
                file.appendText("\n[export failed] ${e.message}\n")
            }
            withContext(Dispatchers.Main) {
                exportLogsButton.isEnabled = true
                exportLogsButton.text = "EXPORT LOGS"
                try {
                    val uri = FileProvider.getUriForFile(
                        this@MainActivity, "${packageName}.fileprovider", file
                    )
                    val share = Intent(Intent.ACTION_SEND).apply {
                        type = "text/plain"
                        putExtra(Intent.EXTRA_STREAM, uri)
                        putExtra(Intent.EXTRA_SUBJECT, "BioShock 1 logs ($stamp)")
                        clipData = android.content.ClipData.newUri(contentResolver, "logs", uri)
                        addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    }
                    startActivity(Intent.createChooser(share, "Share logs"))
                } catch (e: Exception) {
                    Toast.makeText(
                        this@MainActivity, "Share failed: ${e.message}", Toast.LENGTH_LONG
                    ).show()
                }
            }
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) applyImmersiveFullscreen()
    }

    override fun onDestroy() {
        hudHandler.removeCallbacks(fpsRunnable)
        audioFocusRequest?.let {
            (getSystemService(Context.AUDIO_SERVICE) as AudioManager).abandonAudioFocusRequest(it)
        }
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    private fun applyImmersiveFullscreen() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let { ctrl ->
                ctrl.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                ctrl.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            )
        }
    }

    /** Resolution actually rendered when Perf Mode steps the requested preset down a tier. */
    private fun effectiveRes(): String = internalRes

    private fun updateSettingButtons() {
        val isCustom = driverMode == "custom"
        driverButton.text = if (isCustom) "Driver: Custom GLES3" else "Driver: Vulkan"
        scaleButton.text = if (isCustom) {
            "Internal: $internalRes (Live)" +
                if (perfModeEnabled && effectiveRes() != internalRes) " -> ${effectiveRes()} perf" else ""
        } else {
            if (vkScale == 2) "Scale: 2x (1440p)" else "Scale: 1x (720p)"
        }
        upscaleButton.text = if (upscaleMode == "lanczos") "Filter: Lanczos" else "Filter: Bilinear"
        fxaaButton.visibility = if (isCustom) View.GONE else View.VISIBLE
        // Perf Mode is now wired into the custom renderer too (lower res/filter/aniso).
        perfButton.visibility = View.VISIBLE
        fxaaButton.text = if (fxaaEnabled) "AA: FXAA" else "AA: Off"
        perfButton.text = if (perfModeEnabled) "Perf: ON (${effectiveRes()})" else "Perf: OFF"
    }

    private fun updateStatus() {
        val xex = File(gameDir, "default.xex")
        val hleCount = NativeBridge.nativeHleImportCount()
        status.text = buildString {
            appendLine("BioShock 1 - Native Static ReComp (Android -O2)")
            appendLine("Engine: ${if (hleCount > 0) "loaded" else "MISSING"} | XEX: ${if (xex.exists()) "${xex.length()} bytes" else "not imported"}")
            appendLine("Custom GLES 3.2 renderer supports live 360p-1440p internal resolution.")
        }
    }

    private fun openDocument() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        startActivityForResult(intent, REQUEST_IMPORT)
    }

    @Deprecated("Deprecated but required for SAF compatibility on API 28")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_IMPORT || resultCode != Activity.RESULT_OK) return
        val uri = data?.data ?: return
        val name = queryName(uri) ?: "game.iso"

        CoroutineScope(Dispatchers.Main).launch {
            importButton.isEnabled = false
            status.text = "Copying $name (this can take a few minutes)..."
            val ok = withContext(Dispatchers.IO) {
                try {
                    val iso = File(gameDir, "BioShock.iso")
                    contentResolver.openInputStream(uri)!!.use { input ->
                        iso.outputStream().use { output -> input.copyTo(output, 1 shl 20) }
                    }
                    Xdvdfs(iso).use { disc ->
                        val entry = disc.find("default.xex")
                            ?: throw IllegalArgumentException("default.xex not found in ISO")
                        disc.extract(entry, File(gameDir, "default.xex"))
                        disc.extractAll(gameDir) { done, total, bytes, totalBytes ->
                            if (done % 16 == 0 || done == total) {
                                runOnUiThread {
                                    status.text = "Extracting game data: $done/$total files " +
                                        "(${bytes / (1024 * 1024)}/${totalBytes / (1024 * 1024)} MB)"
                                }
                            }
                        }
                    }
                    true
                } catch (e: Exception) {
                    e.printStackTrace()
                    false
                }
            }
            importButton.isEnabled = true
            Toast.makeText(
                this@MainActivity,
                if (ok) "ISO imported; default.xex extracted" else "Import failed (see logcat)",
                Toast.LENGTH_LONG,
            ).show()
            updateStatus()
        }
    }

    private fun queryName(uri: Uri): String? {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (i >= 0) return c.getString(i)
            }
        }
        return uri.lastPathSegment
    }

    private fun makeHudPill(text: String): TextView = TextView(this).apply {
        this.text = text
        textSize = 11f
        setTextColor(Color.argb(230, 235, 245, 255))
        setBackgroundColor(Color.argb(130, 14, 22, 34))
        setPadding(16, 8, 16, 8)
        isFocusable = false
        isFocusableInTouchMode = false
    }

    private fun pushMergedControls() {
        for (i in 0 until 8) {
            mergedAxes[i] = if (abs(padAxes[i]) >= abs(touchAxes[i])) padAxes[i] else touchAxes[i]
        }
        for (i in 0 until 32) {
            mergedButtons[i] = if (touchButtons[i].toInt() != 0 || padButtons[i].toInt() != 0) 1 else 0
        }
        NativeBridge.nativeSetControls(mergedAxes, mergedButtons)
    }

    private fun syncPadCompoundButtons() {
        val dpad = padDpadKeyMask or padDpadHatMask
        padButtons[12] = if ((dpad and 1) != 0) 1 else 0
        padButtons[13] = if ((dpad and 2) != 0) 1 else 0
        padButtons[14] = if ((dpad and 4) != 0) 1 else 0
        padButtons[15] = if ((dpad and 8) != 0) 1 else 0
        padButtons[6] = if (padLtKey || padLtAxis) 1 else 0
        padButtons[7] = if (padRtKey || padRtAxis) 1 else 0
    }

    private fun isGamepadSource(source: Int): Boolean {
        val mask = InputDevice.SOURCE_GAMEPAD or InputDevice.SOURCE_JOYSTICK
        return (source and mask) != 0
    }

    private fun applyStickDeadzone(raw: Float, device: InputDevice?, axis: Int, source: Int): Float {
        val flat = device?.getMotionRange(axis, source)?.flat ?: 0f
        val dz = max(flat, 0.10f).coerceAtMost(0.30f)
        val mag = abs(raw)
        if (mag <= dz) return 0f
        return (sign(raw) * ((mag - dz) / (1f - dz))).coerceIn(-1f, 1f)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (surface != null) {
            val fromPad = isGamepadSource(event.source) ||
                ((event.source and InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD)
            val keyCode = event.keyCode
            val isPadButtonKey = KeyEvent.isGamepadButton(keyCode)
            if (fromPad || isPadButtonKey) {
                val down = event.action == KeyEvent.ACTION_DOWN
                val up = event.action == KeyEvent.ACTION_UP
                if (!down && !up) return true
                if (down && event.repeatCount > 0) return true
                var handled = true
                when (keyCode) {
                    KeyEvent.KEYCODE_BUTTON_A -> padButtons[0] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_B -> padButtons[1] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_X -> padButtons[2] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_Y -> padButtons[3] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_L1 -> padButtons[4] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_R1 -> padButtons[5] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_L2 -> padLtKey = down
                    KeyEvent.KEYCODE_BUTTON_R2 -> padRtKey = down
                    KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> {
                        if (keyCode == KeyEvent.KEYCODE_BACK && !fromPad) {
                            handled = false
                        } else {
                            padButtons[8] = if (down) 1 else 0
                        }
                    }
                    KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_MENU -> {
                        if (keyCode == KeyEvent.KEYCODE_MENU && !fromPad) {
                            handled = false
                        } else {
                            padButtons[9] = if (down) 1 else 0
                        }
                    }
                    KeyEvent.KEYCODE_BUTTON_THUMBL -> padButtons[10] = if (down) 1 else 0
                    KeyEvent.KEYCODE_BUTTON_THUMBR -> padButtons[11] = if (down) 1 else 0
                    KeyEvent.KEYCODE_DPAD_UP -> {
                        padDpadKeyMask = if (down) (padDpadKeyMask or 1) else (padDpadKeyMask and 1.inv())
                    }
                    KeyEvent.KEYCODE_DPAD_DOWN -> {
                        padDpadKeyMask = if (down) (padDpadKeyMask or 2) else (padDpadKeyMask and 2.inv())
                    }
                    KeyEvent.KEYCODE_DPAD_LEFT -> {
                        padDpadKeyMask = if (down) (padDpadKeyMask or 4) else (padDpadKeyMask and 4.inv())
                    }
                    KeyEvent.KEYCODE_DPAD_RIGHT -> {
                        padDpadKeyMask = if (down) (padDpadKeyMask or 8) else (padDpadKeyMask and 8.inv())
                    }
                    KeyEvent.KEYCODE_BUTTON_MODE -> {
                        // Consume Xbox Guide button in-game so it doesn't move focus to HUD pills.
                    }
                    else -> handled = false
                }
                if (handled) {
                    syncPadCompoundButtons()
                    pushMergedControls()
                    return true
                }
            }
        }
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (surface != null && isGamepadSource(event.source) && event.action == MotionEvent.ACTION_MOVE) {
            val dev = event.device
            val src = event.source
            // Left stick (negate Y so +1 = up, matching XInput & TouchControlsView).
            padAxes[0] = applyStickDeadzone(event.getAxisValue(MotionEvent.AXIS_X), dev, MotionEvent.AXIS_X, src)
            padAxes[1] = -applyStickDeadzone(event.getAxisValue(MotionEvent.AXIS_Y), dev, MotionEvent.AXIS_Y, src)

            // Right stick: Xbox Bluetooth/USB uses AXIS_Z / AXIS_RZ on Android; fallback to RX / RY.
            val hasZ = dev?.getMotionRange(MotionEvent.AXIS_Z, src) != null &&
                dev.getMotionRange(MotionEvent.AXIS_RZ, src) != null
            val rxAxis = if (hasZ) MotionEvent.AXIS_Z else MotionEvent.AXIS_RX
            val ryAxis = if (hasZ) MotionEvent.AXIS_RZ else MotionEvent.AXIS_RY
            padAxes[2] = applyStickDeadzone(event.getAxisValue(rxAxis), dev, rxAxis, src)
            padAxes[3] = -applyStickDeadzone(event.getAxisValue(ryAxis), dev, ryAxis, src)

            // Triggers: Xbox Bluetooth exposes AXIS_LTRIGGER/RTRIGGER or AXIS_BRAKE/GAS.
            val lt = max(
                event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_BRAKE),
            )
            val rt = max(
                event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                event.getAxisValue(MotionEvent.AXIS_GAS),
            )
            padLtAxis = lt > 0.15f
            padRtAxis = rt > 0.15f

            // D-Pad hat axes (standard on Xbox Bluetooth controllers on Android).
            val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
            var hatMask = 0
            if (hatY < -0.5f) hatMask = hatMask or 1
            if (hatY > 0.5f) hatMask = hatMask or 2
            if (hatX < -0.5f) hatMask = hatMask or 4
            if (hatX > 0.5f) hatMask = hatMask or 8
            padDpadHatMask = hatMask

            syncPadCompoundButtons()
            pushMergedControls()
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }

    private var audioFocusRequest: AudioFocusRequest? = null

    /** Hold audio focus while the game runs so the system stops stealing/rerouting
     *  the AAudio stream (Samsung MMAP route changes were killing it randomly). */
    private fun requestAudioFocus() {
        val am = getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val attrs = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME)
            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
            .build()
        val req = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attrs)
            .build()
        audioFocusRequest = req
        am.requestAudioFocus(req)
    }

    private fun startGame() {
        if (surface != null) return
        if (!File(gameDir, "default.xex").exists()) {
            Toast.makeText(this, "Import the ISO first", Toast.LENGTH_SHORT).show()
            return
        }
        requestAudioFocus()
        // Mirror renderer logs to a file so EXPORT LOGS works without logcat access.
        val runtimeLog = File(getExternalFilesDir(null), "bs1-runtime.log")
        runtimeLog.writeText("")
        NativeBridge.nativeSetLogFile(runtimeLog.absolutePath)
        surface = GameSurfaceView(this, null).apply {
            setGameRoot(gameDir.absolutePath)
            setRenderConfig(driverMode, internalRes, vkScale, upscaleMode, fxaaEnabled, perfModeEnabled)
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            )
        }
        container.addView(surface)

        val controls = TouchControlsView(this).apply {
            isFocusable = false
            isFocusableInTouchMode = false
            listener = TouchControlsView.Listener { axes, buttons ->
                axes.copyInto(touchAxes, 0, 0, minOf(axes.size, touchAxes.size))
                buttons.copyInto(touchButtons, 0, 0, minOf(buttons.size, touchButtons.size))
                pushMergedControls()
            }
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            )
        }
        touchOverlay = controls
        container.addView(controls)

        val hudBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(16, 12, 16, 12)
            isFocusable = false
            descendantFocusability = ViewGroup.FOCUS_BLOCK_DESCENDANTS
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP or Gravity.START,
            )
        }
        val fps = makeHudPill("-- FPS").apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { marginEnd = 8 }
        }
        hudBar.addView(fps)
        fpsView = fps

        if (driverMode == "custom") {
            val resBtn = makeHudPill("RES: ${effectiveRes()}").apply {
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                ).apply { marginEnd = 8 }
                setOnClickListener {
                    val idx = resPresets.indexOf(internalRes).coerceAtLeast(0)
                    internalRes = resPresets[(idx + 1) % resPresets.size]
                    prefs.edit().putString("internal_res", internalRes).apply()
                    NativeBridge.nativeSetInternalResolution(effectiveRes())
                    text = "RES: ${effectiveRes()}"
                }
            }
            hudBar.addView(resBtn)
            liveResBtn = resBtn
        }

        val filterBtn = makeHudPill(if (upscaleMode == "lanczos") "LANCZOS" else "BILINEAR").apply {
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply { marginEnd = 8 }
            setOnClickListener {
                upscaleMode = if (upscaleMode == "lanczos") "bilinear" else "lanczos"
                prefs.edit().putString("upscale_mode", upscaleMode).apply()
                NativeBridge.nativeSetUpscaleMode(upscaleMode)
                text = if (upscaleMode == "lanczos") "LANCZOS" else "BILINEAR"
            }
        }
        val touchBtn = makeHudPill("TOUCH: ON").apply {
            setOnClickListener {
                val nextVisible = touchOverlay?.visibility != View.VISIBLE
                touchOverlay?.visibility = if (nextVisible) View.VISIBLE else View.GONE
                text = if (nextVisible) "TOUCH: ON" else "TOUCH: OFF"
            }
        }
        liveFilterBtn = filterBtn
        liveTouchBtn = touchBtn
        hudBar.addView(filterBtn)
        hudBar.addView(touchBtn)
        container.addView(hudBar)

        launcherPanel.visibility = View.GONE
        applyImmersiveFullscreen()

        lastPresentCount = 0L
        lastFpsTimeNs = 0L
        hudHandler.postDelayed(fpsRunnable, 1000)
    }

    companion object {
        private const val REQUEST_IMPORT = 42
    }
}
