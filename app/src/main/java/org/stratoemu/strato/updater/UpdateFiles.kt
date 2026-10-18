// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.zip.CRC32
import java.util.zip.ZipFile

/** Files become installable only after a complete transfer/extraction. */
object UpdateFiles {
    const val MAX_BYTES = 2L * 1024 * 1024 * 1024

    fun copy(input: InputStream, target: File, expectedBytes: Long,
             checkCancelled: () -> Unit, progress: (Long, Long) -> Unit,
             expectedCrc: Long = -1): File {
        val partial = File(target.path + ".part")
        try {
            if (expectedBytes > MAX_BYTES) throw IOException("Update exceeds size limit")
            if (!target.parentFile!!.isDirectory && !target.parentFile!!.mkdirs())
                throw IOException("Cannot create update directory")
            var count = 0L
            var lastReport = 0L
            val crc = CRC32()
            progress(0, expectedBytes)
            partial.outputStream().use { output ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    checkCancelled()
                    val size = input.read(buffer)
                    if (size < 0) break
                    if (size == 0) continue
                    count += size
                    if (count > MAX_BYTES || (expectedBytes >= 0 && count > expectedBytes))
                        throw IOException("Unexpected update size")
                    output.write(buffer, 0, size)
                    if (expectedCrc >= 0) crc.update(buffer, 0, size)
                    val now = System.nanoTime()
                    if (now - lastReport >= 100_000_000L) {
                        progress(count, expectedBytes)
                        lastReport = now
                    }
                }
                checkCancelled()
                if (count == 0L || (expectedBytes >= 0 && count != expectedBytes))
                    throw IOException("Empty or incomplete update")
                if (expectedCrc >= 0 && crc.value != expectedCrc)
                    throw IOException("Corrupt APK in update archive")
                output.flush()
                output.fd.sync()
            }
            checkCancelled()
            if (!partial.renameTo(target)) throw IOException("Cannot finalize update file")
            progress(count, expectedBytes)
            return target
        } finally {
            partial.delete()
        }
    }

    fun extractApk(archive: File, target: File, checkCancelled: () -> Unit,
                   progress: (Long, Long) -> Unit): File = ZipFile(archive).use { zip ->
        // The central directory gives sizes without decompressing the archive twice.
        val entries = zip.entries()
        var apk: java.util.zip.ZipEntry? = null
        while (entries.hasMoreElements()) {
            checkCancelled()
            val entry = entries.nextElement()
            if (!entry.isDirectory && entry.name.endsWith(".apk", ignoreCase = true)) {
                if (apk != null) throw IOException("Update archive contains multiple APKs")
                apk = entry
            }
        }
        val entry = apk ?: throw IOException("Update archive contains no APK")
        // Use our destination, never the archive entry name.
        zip.getInputStream(entry).use { input ->
            copy(input, target, entry.size, checkCancelled, progress, entry.crc)
        }
    }
}
