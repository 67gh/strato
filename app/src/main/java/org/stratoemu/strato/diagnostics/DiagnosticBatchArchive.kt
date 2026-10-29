/* SPDX-License-Identifier: MPL-2.0 */

package org.stratoemu.strato.diagnostics

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Sequential disk streaming: keep at most one temporary session ZIP beside the batch. */
internal object DiagnosticBatchArchive {
    private const val MAX_BYTES = 512L * 1024 * 1024

    class TooLargeException : IOException("Diagnostic batch exceeds 512 MiB")

    fun create(destination : File, sessions : List<File>, exportSession : (File) -> File,
               checkCancelled : () -> Unit = {}, maxBytes : Long = MAX_BYTES) : File {
        require(sessions.isNotEmpty()) { "Select at least one diagnostic" }
        require(maxBytes > 0)
        checkCancelled()
        if (!destination.isDirectory && !destination.mkdirs())
            throw IOException("Cannot create diagnostic export directory")
        val output = File(destination, "diagnostics-${UUID.randomUUID()}.zip")
        val partial = File(destination, output.name + ".partial")
        try {
            ZipOutputStream(BoundedOutput(FileOutputStream(partial), maxBytes).buffered()).use { zip ->
                // Inner ZIPs are already compressed; do not spend CPU compressing them again.
                zip.setLevel(0)
                sessions.forEachIndexed { index, session ->
                    checkCancelled()
                    val diagnostic = exportSession(session)
                    try {
                        checkCancelled()
                        // Numbered names avoid collisions and keep session names out of ZIP paths.
                        // Each inner session.json / manifest.json retains its original identity.
                        zip.putNextEntry(ZipEntry("diagnostic-${index + 1}.zip"))
                        diagnostic.inputStream().use { input ->
                            val buffer = ByteArray(64 * 1024)
                            while (true) {
                                checkCancelled()
                                val count = input.read(buffer)
                                if (count < 0) break
                                zip.write(buffer, 0, count)
                            }
                        }
                        zip.closeEntry()
                    } finally {
                        // These intermediates are created by this call, never the source evidence.
                        diagnostic.delete()
                    }
                }
            }
            checkCancelled()
            if (!partial.renameTo(output)) throw IOException("Cannot finalize diagnostic batch")
            return output
        } finally {
            partial.delete()
        }
    }

    private class BoundedOutput(private val target : OutputStream, private val limit : Long) : OutputStream() {
        private var count = 0L

        override fun write(value : Int) {
            if (count >= limit) throw TooLargeException()
            target.write(value)
            count++
        }

        override fun write(buffer : ByteArray, offset : Int, length : Int) {
            if (length > limit - count) throw TooLargeException()
            target.write(buffer, offset, length)
            count += length
        }

        override fun flush() = target.flush()
        override fun close() = target.close()
    }
}
