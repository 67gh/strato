// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.updater

import java.io.File
import java.io.IOException
import java.util.zip.CRC32
import java.util.zip.ZipFile

/** Detect the payload, not its extension. Never repack an APK (which would break its signature). */
object APKPayload {
    fun prepare(download: File, target: File, check: () -> Unit,
                extracting: () -> Unit, progress: (Long, Long) -> Unit): File {
        check()
        val direct = ZipFile(download).use { zip ->
            zip.getEntry("AndroidManifest.xml")?.let { !it.isDirectory } == true
        }
        if (direct) {
            verify(download, check)
            if (!download.renameTo(target)) throw IOException("Cannot finalize APK")
        } else {
            extracting()
            UpdateFiles.extractApk(download, target, check, progress)
            verify(target, check)
        }
        return target
    }

    /** Structural/CRC check only; Android remains responsible for manifest semantics and signing. */
    fun verify(file: File, check: () -> Unit) {
        if (!file.isFile || file.length() == 0L || file.length() > UpdateFiles.MAX_BYTES)
            throw IOException("APK missing, empty or oversized")
        ZipFile(file).use { zip ->
            val manifest = zip.getEntry("AndroidManifest.xml")
            if (manifest == null || manifest.isDirectory || manifest.size <= 0)
                throw IOException("No AndroidManifest.xml at APK root")
            val names = HashSet<String>()
            val buffer = ByteArray(64 * 1024)
            var expanded = 0L
            val entries = zip.entries()
            while (entries.hasMoreElements()) {
                check()
                val entry = entries.nextElement()
                if (!names.add(entry.name)) throw IOException("Duplicate APK entry: ${entry.name}")
                if (entry.isDirectory) continue
                var size = 0L
                val crc = CRC32()
                zip.getInputStream(entry).use { input ->
                    while (true) {
                        check()
                        val n = input.read(buffer)
                        if (n < 0) break
                        size += n
                        expanded += n
                        if (expanded > UpdateFiles.MAX_BYTES) throw IOException("APK expanded size limit")
                        crc.update(buffer, 0, n)
                    }
                }
                if (size != entry.size || crc.value != entry.crc)
                    throw IOException("APK entry corrupted: ${entry.name}")
            }
        }
    }
}
