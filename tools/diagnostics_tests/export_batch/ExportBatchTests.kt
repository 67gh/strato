/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.diagnostics

import android.content.Context
import org.json.JSONObject
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.time.Instant
import java.time.ZoneId
import java.util.concurrent.CancellationException
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

private inline fun <reified T : Exception> fails(block : () -> Unit) {
    try { block() } catch (error : Exception) {
        check(error is T) { "Expected ${T::class.java}, got $error" }
        return
    }
    error("Expected ${T::class.java}")
}

private fun entries(bytes : ByteArray) : Map<String, ByteArray> = buildMap {
    ZipInputStream(ByteArrayInputStream(bytes)).use { zip ->
        while (true) {
            val entry = zip.nextEntry ?: break
            check(put(entry.name, zip.readBytes()) == null)
        }
    }
}

fun main() {
    val root = Files.createTempDirectory("strato-export-tests-").toFile()
    try {
        val context = Context(File(root, "files"), File(root, "cache"))
        val sessions = (1..3).map { index ->
            File(DiagnosticSession.sessionRoot(context), "session-$index").apply {
                check(mkdirs())
                File(this, "session.json").writeText("""{"status":"finished","game_name":"Game $index"}""")
                File(this, "emulation.log").writeText("unique log $index\n")
                File(this, "flight_recorder.jsonl").writeText("{\"event\":$index}\nunfin")
            }
        }
        val cache = File(context.cacheDir, "diagnostics")
        val savedLogs = sessions.map { File(it, "emulation.log").readBytes() }
        val selected = listOf(sessions[2], sessions[0], sessions[2])
        val batch = DiagnosticExporter.exportBatch(context, selected)
        ZipFile(batch).use { zip ->
            check(zip.size() == 2) // Canonical duplicate selections are exported only once.
            listOf(sessions[2], sessions[0]).forEachIndexed { index, session ->
                val content = entries(zip.getInputStream(zip.getEntry("diagnostic-${index + 1}.zip")).readBytes())
                val manifest = JSONObject(content.getValue("manifest.json").toString(Charsets.UTF_8))
                check(manifest.getString("session_id") == session.name)
                check(!manifest.getBoolean("session_active_at_export_start"))
                check(content.getValue("emulation.log").contentEquals(File(session, "emulation.log").readBytes()))
                check(content.getValue("flight_recorder.jsonl").toString(Charsets.UTF_8).endsWith("}\n"))
                check(JSONObject(content.getValue("session.json").toString(Charsets.UTF_8)).getString("status") == "finished")
            }
        }
        check(cache.listFiles()!!.toList() == listOf(batch))
        sessions.forEachIndexed { i, session -> check(File(session, "emulation.log").readBytes().contentEquals(savedLogs[i])) }
        check(batch.delete())
        println("PASS: multi-session ZIP, selected order, deduplication, manifests, intact source logs, temporary cleanup")

        // The existing single-session format remains at the root, without an extra nested ZIP.
        val single = DiagnosticExporter.export(context, sessions[0])
        check("manifest.json" in entries(single.readBytes()))
        check(single.delete())
        DiagnosticFileLock.acquire(sessions[1])!!.use {
            val live = DiagnosticExporter.exportBatch(context, listOf(sessions[1]))
            val inner = entries(entries(live.readBytes()).getValue("diagnostic-1.zip"))
            check(JSONObject(inner.getValue("manifest.json").toString(Charsets.UTF_8)).getBoolean("session_active_at_export_start"))
            check(live.delete())
        }
        println("PASS: single export compatibility and explicitly identified live snapshot")

        fails<IllegalArgumentException> { DiagnosticExporter.exportBatch(context, emptyList()) }
        val malformed = File(DiagnosticSession.sessionRoot(context), "missing-metadata").apply { mkdirs() }
        fails<IllegalArgumentException> { DiagnosticExporter.exportBatch(context, listOf(sessions[0], malformed)) }
        check(cache.listFiles()!!.isEmpty())
        println("PASS: empty selection and later session failure leave no successful or partial ZIP")

        val batchOnly = File(root, "batch-only")
        var exported = 0
        fun makeTemporary(session : File) : File = File(root, "temporary-${session.name}.zip").apply {
            writeBytes(ByteArray(200_000) { (it % 251).toByte() })
            exported++
        }
        fails<DiagnosticBatchArchive.TooLargeException> {
            DiagnosticBatchArchive.create(batchOnly, sessions, ::makeTemporary, maxBytes = 100_000)
        }
        check(exported == 1)
        check(batchOnly.listFiles()!!.isEmpty())
        check(root.listFiles()!!.none { it.name.startsWith("temporary-") })

        // Cancel after the temporary report exists, while streaming it into the outer ZIP.
        var checks = 0
        fails<CancellationException> {
            DiagnosticBatchArchive.create(batchOnly, sessions, ::makeTemporary, checkCancelled = {
                if (++checks == 5) throw CancellationException("test cancellation")
            })
        }
        check(batchOnly.listFiles()!!.isEmpty())
        check(root.listFiles()!!.none { it.name.startsWith("temporary-") })
        println("PASS: bounded streaming and mid-copy cancellation clean up partial and temporary files")

        fun verifyWindow(days : Int, now : String, zone : String, start : String, end : String) {
            val range = diagnosticExportWindow(days, Instant.parse(now), ZoneId.of(zone))
            val first = Instant.parse(start).toEpochMilli()
            val after = Instant.parse(end).toEpochMilli()
            check(range.first == first && range.last == after - 1)
            check(first in range && after - 1 in range && first - 1 !in range && after !in range)
        }
        verifyWindow(1, "2026-10-29T12:00:00Z", "Africa/Libreville", "2026-10-28T23:00:00Z", "2026-10-29T23:00:00Z")
        verifyWindow(7, "2026-10-29T12:00:00Z", "Africa/Libreville", "2026-10-22T23:00:00Z", "2026-10-29T23:00:00Z")
        verifyWindow(1, "2026-03-29T12:00:00Z", "Europe/Paris", "2026-03-28T23:00:00Z", "2026-03-29T22:00:00Z")
        verifyWindow(1, "2026-10-25T12:00:00Z", "Europe/Paris", "2026-10-24T22:00:00Z", "2026-10-25T23:00:00Z")
        verifyWindow(7, "2026-03-30T12:00:00Z", "Europe/Paris", "2026-03-23T23:00:00Z", "2026-03-30T22:00:00Z")
        verifyWindow(7, "2027-01-02T12:00:00Z", "UTC", "2026-12-27T00:00:00Z", "2027-01-03T00:00:00Z")
        fails<IllegalArgumentException> { diagnosticExportWindow(0) }
        println("PASS: local day/week bounds, year boundary, 23/25-hour days, invalid period")
    } finally {
        root.deleteRecursively()
    }
}
