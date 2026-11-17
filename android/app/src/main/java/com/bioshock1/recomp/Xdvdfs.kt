package com.bioshock1.recomp

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.experimental.and

/**
 * Minimal XDVDFS (Xbox DVD File System) reader.
 *
 * Only what the launcher needs: locate the game partition, walk the B-tree
 * root directory, and stream default.xex into app-private storage. The game
 * data is never bundled in the APK - the user imports their own ISO.
 */
class Xdvdfs(private val iso: File) : AutoCloseable {
    private val raf = RandomAccessFile(iso, "r")
    private val base: Long
    private val rootSector: Int
    private val rootSize: Int

    data class Entry(
        val name: String,
        val sector: Int,
        val size: Int,
        val isDir: Boolean,
    )

    init {
        val magic = "MICROSOFT*XBOX*MEDIA".toByteArray(Charsets.US_ASCII)
        var magicOffset = -1L
        for (candidate in longArrayOf(0x0FD90000, 0x0FDA0000, 0x02080000, 0)) {
            raf.seek(candidate)
            val probe = ByteArray(20)
            if (raf.read(probe) == 20 && probe.contentEquals(magic)) {
                magicOffset = candidate
                break
            }
        }
        if (magicOffset < 0) {
            raf.close()
            throw IllegalArgumentException("XDVDFS partition not found (encrypted ISO?)")
        }
        base = if (magicOffset >= 0x10000) magicOffset - 0x10000 else magicOffset
        raf.seek(magicOffset + 20)
        val header = ByteArray(8)
        raf.readFully(header)
        val bb = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
        rootSector = bb.int
        rootSize = bb.int
    }

    private fun parseDir(sector: Int, size: Int): List<Entry> {
        raf.seek(base + sector.toLong() * SECTOR)
        val data = ByteArray(size)
        raf.readFully(data)
        val out = mutableListOf<Entry>()
        val stack = ArrayDeque<Int>()
        val seen = HashSet<Int>()
        stack.add(0)
        while (stack.isNotEmpty()) {
            val off = stack.removeLast()
            if (!seen.add(off) || off + 14 > data.size) continue
            val left = u16(data, off)
            val right = u16(data, off + 2)
            val start = u32(data, off + 4)
            val fsize = u32(data, off + 8)
            val attrs = data[off + 12]
            val nameLen = data[off + 13].toInt()
            if (nameLen == 0 || off + 14 + nameLen > data.size) continue
            val raw = data.copyOfRange(off + 14, off + 14 + nameLen)
            if (raw.any { it < 32 || it >= 127 }) continue
            out.add(
                Entry(
                    name = String(raw, Charsets.US_ASCII),
                    sector = start.toInt(),
                    size = fsize.toInt(),
                    isDir = (attrs and 0x10.toByte()) != 0.toByte(),
                )
            )
            if (left != 0 && left * 4 < data.size) stack.add(left * 4)
            if (right != 0 && right * 4 < data.size) stack.add(right * 4)
        }
        return out
    }

    /** Find a file by case-insensitive path suffix. */
    fun find(name: String): Entry? {
        val queue = ArrayDeque<Pair<Int, Int>>()
        queue.add(rootSector to rootSize)
        while (queue.isNotEmpty()) {
            val (sector, size) = queue.removeFirst()
            for (e in parseDir(sector, size)) {
                if (e.isDir) {
                    queue.add(e.sector to e.size)
                } else if (e.name.equals(name, ignoreCase = true)) {
                    return e
                }
            }
        }
        return null
    }

    fun extract(entry: Entry, dest: File) {
        dest.parentFile?.mkdirs()
        raf.seek(base + entry.sector.toLong() * SECTOR)
        var remaining = entry.size
        dest.outputStream().use { out ->
            val buf = ByteArray(1 shl 20)
            while (remaining > 0) {
                val read = raf.read(buf, 0, minOf(remaining, buf.size))
                if (read <= 0) throw IllegalStateException("short read from ISO")
                out.write(buf, 0, read)
                remaining -= read
            }
        }
    }

    /** All files in the disc with their relative paths and total byte count. */
    fun listFiles(): List<Pair<String, Entry>> {
        val out = mutableListOf<Pair<String, Entry>>()
        val stack = ArrayDeque<Triple<Int, Int, String>>()
        stack.add(Triple(rootSector, rootSize, ""))
        while (stack.isNotEmpty()) {
            val (sector, size, prefix) = stack.removeFirst()
            for (e in parseDir(sector, size)) {
                val path = if (prefix.isEmpty()) e.name else "$prefix/${e.name}"
                if (e.isDir) stack.add(Triple(e.sector, e.size, path))
                else out.add(path to e)
            }
        }
        return out
    }

    /**
     * Extract the complete game tree into [destDir], preserving paths.
     * [onProgress] receives (filesDone, totalFiles, bytesDone, totalBytes).
     */
    fun extractAll(destDir: File, onProgress: (Int, Int, Long, Long) -> Unit = { _, _, _, _ -> }) {
        val files = listFiles()
        val totalBytes = files.sumOf { it.second.size.toLong() }
        var bytesDone = 0L
        files.forEachIndexed { index, (path, entry) ->
            val safe = path.split('/').joinToString("/") { it.replace('\\', '_') }
            extract(entry, File(destDir, safe))
            bytesDone += entry.size
            onProgress(index + 1, files.size, bytesDone, totalBytes)
        }
    }

    override fun close() = raf.close()

    private companion object {
        const val SECTOR = 2048
        fun u16(b: ByteArray, o: Int) =
            (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)

        fun u32(b: ByteArray, o: Int) =
            (b[o].toLong() and 0xFF) or
                ((b[o + 1].toLong() and 0xFF) shl 8) or
                ((b[o + 2].toLong() and 0xFF) shl 16) or
                ((b[o + 3].toLong() and 0xFF) shl 24)
    }
}
