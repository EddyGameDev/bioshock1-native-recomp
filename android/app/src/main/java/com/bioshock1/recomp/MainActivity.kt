package com.bioshock1.recomp

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : Activity() {
    private lateinit var status: TextView
    private lateinit var importButton: Button
    private lateinit var startButton: Button
    private lateinit var container: FrameLayout
    private var surface: GameSurfaceView? = null

    private val gameDir: File by lazy { File(filesDir, "game").apply { mkdirs() } }

    // SDL3's Android message box path calls this reflectively on the context
    // Activity. For this headless boot test just log the message and return a
    // neutral button ID instead of showing a native dialog.
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
        org.libsdl.app.SDLActivity.setContext(this)
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.status)
        importButton = findViewById(R.id.importButton)
        startButton = findViewById(R.id.startButton)
        container = findViewById(R.id.surfaceContainer)

        importButton.setOnClickListener { openDocument() }
        startButton.setOnClickListener { startGame() }
        updateStatus()
    }

    private fun updateStatus() {
        val xex = File(gameDir, "default.xex")
        // extractNativeLibs=false loads the .so directly from the APK, so
        // nativeLibraryDir may be empty. A successful JNI call is the real proof.
        val hleCount = NativeBridge.nativeHleImportCount()
        status.text = buildString {
            appendLine("BioShock 1 - native static recomp (preview)")
            appendLine("Engine library: ${if (hleCount > 0) "loaded" else "MISSING"}")
            appendLine("XEX extracted: ${if (xex.exists()) "${xex.length()} bytes" else "no"}")
            appendLine("HLE imports implemented: $hleCount")
            appendLine()
            appendLine("Import your own ISO, then tap Start.")
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
                        // Full game tree: the guest filesystem needs the Unreal
                        // packages and FMOD banks, not just the executable.
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

    private fun startGame() {
        if (surface != null) return
        if (!File(gameDir, "default.xex").exists()) {
            Toast.makeText(this, "Import the ISO first", Toast.LENGTH_SHORT).show()
            return
        }
        surface = GameSurfaceView(this, null).apply {
            setGameRoot(gameDir.absolutePath)
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            )
        }
        container.addView(surface)
        val controls = TouchControlsView(this).apply {
            listener = TouchControlsView.Listener { axes, buttons ->
                NativeBridge.nativeSetControls(axes, buttons)
            }
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            )
        }
        container.addView(controls)
        importButton.visibility = View.GONE
        startButton.visibility = View.GONE
        status.visibility = View.GONE
    }

    companion object {
        private const val REQUEST_IMPORT = 42
    }
}
