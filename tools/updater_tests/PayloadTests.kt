// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.updater

import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.util.concurrent.CancellationException
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

private fun zip(file: File, entries: List<Pair<String, ByteArray>>) {
    ZipOutputStream(file.outputStream()).use { out ->
        entries.forEach { (name, data) ->
            out.putNextEntry(ZipEntry(name)); out.write(data); out.closeEntry()
        }
    }
}
private fun rejected(action: () -> Unit) {
    try { action(); error("Expected IOException") } catch (_: IOException) { }
}
fun main() {
    val dir = Files.createTempDirectory("strato-payload-").toFile()
    try {
        val source = File(dir, "download.payload")
        val target = File(dir, "update.apk")
        // Synthetic ZIP fixtures, NOT installable APKs. Tests structure and byte preservation only.
        val entries = listOf("AndroidManifest.xml" to byteArrayOf(3, 0, 8, 0),
            "classes.dex" to ByteArray(4096) { it.toByte() })
        zip(source, entries)
        val apk = source.readBytes()
        var extracted = false
        APKPayload.prepare(source, target, {}, { extracted = true }) { _, _ -> }
        check(!extracted && target.readBytes().contentEquals(apk))
        println("PASS direct APK bytes preserved exactly")
        zip(source, listOf("nested/build.apk" to apk))
        APKPayload.prepare(source, target, {}, { extracted = true }) { _, _ -> }
        check(extracted && target.readBytes().contentEquals(apk))
        println("PASS legacy wrapper detected without trusting extension or archive flag")
        for (text in listOf("<html>Error</html>", "", "{\"message\":\"Not Found\"}")) {
            source.writeText(text)
            rejected { APKPayload.prepare(source, target, {}, {}) { _, _ -> } }
        }
        println("PASS HTML, JSON, empty response rejected")
        zip(source, listOf("readme.txt" to byteArrayOf(1)))
        rejected { APKPayload.prepare(source, target, {}, {}) { _, _ -> } }
        zip(source, listOf("a.apk" to apk, "b.apk" to apk))
        rejected { APKPayload.prepare(source, target, {}, {}) { _, _ -> } }
        zip(source, listOf("AndroidManifest.xml" to byteArrayOf()))
        rejected { APKPayload.verify(source, {}) }
        println("PASS absent manifest, ambiguous archive, empty manifest rejected")
        source.writeBytes(apk.copyOf(apk.size - 30))
        rejected { APKPayload.verify(source, {}) }
        val corrupt = apk.copyOf()
        val central = (0 until corrupt.size - 4).first {
            corrupt[it] == 0x50.toByte() && corrupt[it+1] == 0x4b.toByte() &&
                corrupt[it+2] == 1.toByte() && corrupt[it+3] == 2.toByte()
        }
        corrupt[central + 16] = (corrupt[central + 16].toInt() xor 255).toByte()
        source.writeBytes(corrupt)
        rejected { APKPayload.verify(source, {}) }
        println("PASS truncation and CRC corruption rejected")
        source.writeBytes(apk)
        var calls = 0
        try {
            APKPayload.verify(source) { if (++calls == 3) throw CancellationException() }
            error("Expected cancellation")
        } catch (_: CancellationException) { }
        check(source.readBytes().contentEquals(apk))
        println("PASS cancellation preserves original bytes")
    } finally { dir.deleteRecursively() }
}
