/*
 * SPDX-License-Identifier: MPL-2.0
 */

package org.stratoemu.strato.diagnostics

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream
import java.io.RandomAccessFile
import java.nio.file.Files
import java.nio.file.LinkOption
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/** Export a single session with explicit provenance and a finite byte budget. Run off the UI thread. */
object DiagnosticExporter {
    private const val MIB = 1024L * 1024L
    private const val ZIP_LIMIT = 64 * MIB
    // Reserve space for the manifest and ZIP headers, including worst-case deflate overhead.
    private const val PAYLOAD_LIMIT = 62 * MIB

    private data class Evidence(val name : String, val limit : Long, val description : String, val structured : Boolean = false)

    // An allowlist intentionally excludes keys, firmware, saves, arbitrary app files and global logs.
    private val evidence = listOf(
        Evidence("session.json", MIB, "Launch identity, process and lifecycle", true),
        Evidence("device.json", MIB, "Device, app, memory and firmware version at launch", true),
        Evidence("settings.json", MIB, "Global, game and effective launch settings", true),
        Evidence("collector.json", MIB, "Collector availability, errors and retention", true),
        Evidence("native.json", MIB, "Native initialization and actual compiled JIT status", true),
        Evidence("vulkan.json", 2 * MIB, "Vulkan driver, capabilities, quirks and memory snapshot", true),
        Evidence("native_failure.json", MIB, "Exception caught at the JNI execution boundary", true),
        Evidence("crash.log", MIB, "Uncaught Kotlin/Java exception in this session"),
        Evidence("gpu_fault.log", 4 * MIB, "Native GPU fault evidence recorded in this session"),
        Evidence("nce_fallback.jsonl", 8 * MIB, "Native JIT fallback records in this session"),
        Evidence("timeline.previous.jsonl", MIB, "Previous chronological chunk of host counters"),
        Evidence("timeline.jsonl", MIB, "Latest chronological chunk of host counters"),
        Evidence("logcat.previous.log", 4 * MIB, "Previous logcat chunk, restricted to this session's process and start time"),
        Evidence("logcat.log", 4 * MIB, "Latest logcat chunk, restricted to this session's process and start time"),
        Evidence("emulation.log", 24 * MIB, "Native emulation log, including existing watchdog traces")
    )

    @JvmStatic
    fun export(context : Context, sessionDirectory : File) : File {
        val session = validatedDirectory(context, sessionDirectory)
        val sessionMetadata = File(session, "session.json")
        require(sessionMetadata.isFile && sessionMetadata.canonicalFile.parentFile == session && sessionMetadata.canonicalFile.name == "session.json") {
            "The selected directory has no valid session metadata"
        }

        val destination = File(context.cacheDir, "diagnostics").canonicalFile
        if (!destination.isDirectory && !destination.mkdirs()) throw IOException("Cannot create diagnostic export directory")
        val output = File(destination, "diagnostic-${session.name}-${UUID.randomUUID().toString().take(8)}.zip")
        val partial = File(destination, output.name + ".partial")
        // Hold an inactive session's lock during export. A frozen but still live process remains
        // exportable as an explicitly non-atomic snapshot; its logs are never deleted or reset.
        val sessionLock = DiagnosticFileLock.acquire(session)
        val liveSnapshot = sessionLock == null
        try {
            val manifest = JSONObject().put("schema_version", 1).put("exported_at_utc", utcNow())
                .put("session_id", session.name).put("session_active_at_export_start", liveSnapshot)
                .put("snapshot_consistency", if (liveSnapshot) "live_files_may_change_between_reads" else "inactive_session")
                .put("archive_size_limit_bytes", ZIP_LIMIT).put("payload_size_limit_bytes", PAYLOAD_LIMIT)
                .put("sources", "Only files in this selected launch directory; no global log is copied")
                .put("excluded", JSONArray(listOf("prod.keys", "title.keys", "firmware files", "fonts", "ROMs", "saves", "other sessions")))
                .put("interpretation", "Missing evidence is not proof that a subsystem succeeded or was unused. native_returned is not a success verdict.")
                .put("limitations", limitations())
            val availability = JSONArray()
            var payloadBytes = 0L
            ZipOutputStream(LimitedOutput(FileOutputStream(partial), ZIP_LIMIT).buffered()).use { zip ->
                for (entry in evidence) {
                    val record = JSONObject().put("file", entry.name).put("description", entry.description)
                        .put("per_file_limit_bytes", entry.limit)
                    availability.put(record)
                    val file = File(session, entry.name)
                    if (!file.exists()) {
                        record.put("status", "missing").put("included_bytes", 0)
                            .put("reason", missingReason(entry.name))
                        continue
                    }
                    if (!file.isFile || file.canonicalFile.parentFile != session || file.canonicalFile.name != entry.name) {
                        record.put("status", "omitted").put("included_bytes", 0).put("reason", "Not a regular file inside this session")
                        continue
                    }
                    val remaining = (PAYLOAD_LIMIT - payloadBytes).coerceAtLeast(0)
                    if (remaining == 0L) {
                        record.put("status", "omitted").put("included_bytes", 0).put("reason", "Archive payload budget exhausted")
                        continue
                    }
                    val source = try {
                        RandomAccessFile(file, "r")
                    } catch (e : IOException) {
                        record.put("status", "unreadable").put("included_bytes", 0).put("error", safeError(e))
                        continue
                    }
                    source.use {
                        val sourceSize = source.length()
                        record.put("source_size_at_open_bytes", sourceSize)
                        val limit = minOf(entry.limit, remaining)
                        if (entry.structured) {
                            // Never turn a JSON object into an apparently valid tail fragment.
                            if (sourceSize > limit) {
                                record.put("status", "omitted").put("included_bytes", 0)
                                    .put("reason", "Structured JSON exceeds available budget; not truncated")
                            } else {
                                val bytes = ByteArray(sourceSize.toInt())
                                val readError = try { source.readFully(bytes); null } catch (e : IOException) { e }
                                if (readError != null) {
                                    record.put("status", "unreadable").put("included_bytes", 0).put("error", safeError(readError))
                                } else {
                                    val parseError = try { JSONObject(String(bytes, Charsets.UTF_8)); null } catch (e : Exception) { e }
                                    zip.putNextEntry(ZipEntry(entry.name))
                                    zip.write(bytes)
                                    zip.closeEntry()
                                    payloadBytes += bytes.size
                                    record.put("included_bytes", bytes.size).put("status", if (parseError == null) "included" else "invalid_json_included_raw")
                                    if (parseError != null) record.put("error", safeError(parseError))
                                }
                            }
                        } else {
                            val result = copyTextSnapshot(source, sourceSize, limit, entry.name, zip)
                            payloadBytes += result.getLong("included_bytes")
                            for (key in result.keys()) record.put(key, result.get(key))
                        }
                        record.put("source_size_after_read_bytes", file.length())
                        record.put("source_may_have_changed", file.length() != sourceSize || liveSnapshot)
                    }
                }
                manifest.put("files", availability).put("included_payload_bytes", payloadBytes)
                manifest.put("session_state", readSessionState(session, liveSnapshot))
                val manifestBytes = manifest.toString(2).toByteArray(Charsets.UTF_8)
                if (manifestBytes.size > MIB) throw IOException("Diagnostic manifest exceeds its reserved budget")
                zip.putNextEntry(ZipEntry("manifest.json"))
                zip.write(manifestBytes)
                zip.closeEntry()
            }
            if (!partial.renameTo(output)) throw IOException("Cannot finalize diagnostic ZIP")
            return output
        } finally {
            sessionLock?.close()
            // Only an incomplete export owned by this call is removed; session evidence is retained.
            if (partial.exists()) partial.delete()
        }
    }

    /** Explicit user action only. No automatic retention policy deletes diagnostic evidence. */
    @JvmStatic
    fun deleteSession(context : Context, sessionDirectory : File) {
        val session = validatedDirectory(context, sessionDirectory)
        val lock = DiagnosticFileLock.acquire(session) ?: throw IOException("The session is active or another operation holds its lock")
        try {
            val files = session.listFiles() ?: throw IOException("Cannot list the selected session")
            // Validate the complete list before deleting anything. Do not traverse subdirectories
            // or follow even an in-directory symbolic link supplied by a malformed session.
            for (file in files) {
                if (Files.isSymbolicLink(file.toPath()) || !Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) ||
                    file.canonicalFile.parentFile != session || file.canonicalFile.name != file.name)
                    throw IOException("Session contains an unsupported entry: ${file.name}")
            }
            for (file in files.filter { it.name != "session.json" && it.name != ".active.lock" }) {
                if (!file.delete()) throw IOException("Cannot delete ${file.name}")
            }
            val metadata = File(session, "session.json")
            if (metadata.exists() && !metadata.delete()) throw IOException("Cannot delete session metadata")
            val lockFile = File(session, ".active.lock")
            if (lockFile.exists() && !lockFile.delete()) throw IOException("Cannot delete session lock")
            if (!session.delete()) throw IOException("Cannot delete session directory")
        } finally {
            lock.close()
        }
    }

    private fun validatedDirectory(context : Context, directory : File) : File {
        val root = DiagnosticSession.sessionRoot(context).canonicalFile
        val session = directory.canonicalFile
        if (Files.isSymbolicLink(directory.toPath()) || session.parentFile != root || !session.isDirectory)
            throw IOException("Expected one diagnostic session directory")
        return session
    }

    private fun limitations() = JSONObject()
        .put("diagnosis", "Evidence supports diagnosis; it cannot guarantee a unique cause for every hang")
        .put("gpu_memory", "vulkan.json is an initialization snapshot when available; no allocation/budget timeline in this delivery")
        .put("ipc", "A bounded IPC command/stub ring is not implemented in this delivery; existing native logs may contain IPC messages")
        .put("guest_threads", "Host thread counts are not guest thread states; see emulation.log for existing watchdog traces")
        .put("pc_lr_sampling", "Existing watchdog samples may be present in emulation.log; this delivery adds no structured PC/LR capture or new progress-triggered watchdog")
        .put("draws_dispatches_pipelines", "Periodic counters are unavailable in this delivery")
        .put("logcat", "Finite process-filtered snapshots every 2 seconds; the last interval can be lost on abrupt death, and OS buffer overwrite or clock changes can lose records. System-wide tombstones and restricted buffers may be unavailable")
        .put("retention", "timeline and logcat retain two bounded chunks; older records may be overwritten, see collector.json")
        .put("native_log_retention", "Oversized native logs are tail-truncated in the export; this export does not cap native on-device writers")
        .put("settings", "Launch snapshot only; later live changes are not recorded in this delivery")
        .put("privacy", "Names and profile image fields are redacted from settings; game names and diagnostic logs can still contain paths or user-provided text")

    private fun missingReason(name : String) = when (name) {
        "crash.log" -> "No session-scoped Kotlin/Java uncaught exception was recorded; native crashes may appear in other files"
        "native_failure.json" -> "No exception was recorded at the JNI boundary; fatal signals bypass that boundary"
        "native.json", "vulkan.json" -> "Native capture was unavailable or initialization did not reach this stage"
        "nce_fallback.jsonl" -> "No fallback file was produced; check native.json for compiled JIT availability, absence is not a JIT test"
        "gpu_fault.log" -> "No session GPU fault file was produced; absence does not rule out a GPU failure"
        "timeline.previous.jsonl", "logcat.previous.log" -> "The collector may not have rotated yet; see collector.json"
        "logcat.log", "timeline.jsonl" -> "No retained collector data; see collector.json for availability and errors"
        else -> "File was not recorded or is unavailable in this launch"
    }

    /** Keep the newest complete lines, fixing both ends so JSONL remains parseable after a kill. */
    private fun copyTextSnapshot(source : RandomAccessFile, size : Long, limit : Long, name : String, zip : ZipOutputStream) : JSONObject {
        var start = (size - limit).coerceAtLeast(0)
        var end = size
        try {
            if (start > 0) {
                source.seek(start - 1)
                if (source.read() != '\n'.code) {
                    val buffer = ByteArray(8192)
                    source.seek(start)
                    var cursor = start
                    start = size
                    search@ while (cursor < size) {
                        val count = source.read(buffer, 0, minOf(buffer.size.toLong(), size - cursor).toInt())
                        if (count <= 0) throw IOException("Source changed while finding a complete first line")
                        for (index in 0 until count) {
                            if (buffer[index] == '\n'.code.toByte()) { start = cursor + index + 1; break@search }
                        }
                        cursor += count
                    }
                }
            }
            if (name.endsWith(".jsonl") && end > start) {
                // At most one bounded chunk is scanned. Native JSONL may end mid-write after a crash.
                val buffer = ByteArray(8192)
                var cursor = end
                var completeEnd = start
                search@ while (cursor > start) {
                    val count = minOf(buffer.size.toLong(), cursor - start).toInt()
                    val base = cursor - count
                    source.seek(base)
                    source.readFully(buffer, 0, count)
                    for (index in count - 1 downTo 0) {
                        if (buffer[index] == '\n'.code.toByte()) { completeEnd = base + index + 1; break@search }
                    }
                    cursor = base
                }
                end = completeEnd
            }
            source.seek(start)
        } catch (e : IOException) {
            return JSONObject().put("status", "unreadable").put("included_bytes", 0).put("error", safeError(e))
        }
        val record = JSONObject().put("source_start_offset_bytes", start).put("source_end_offset_bytes", end)
            .put("omitted_prefix_bytes", start).put("omitted_partial_suffix_bytes", size - end)
        var included = 0L
        var readError : IOException? = null
        zip.putNextEntry(ZipEntry(name))
        val buffer = ByteArray(32 * 1024)
        while (included < end - start) {
            val count = try {
                source.read(buffer, 0, minOf(buffer.size.toLong(), end - start - included).toInt())
            } catch (e : IOException) {
                readError = e
                break
            }
            if (count <= 0) { readError = IOException("Source changed or ended before its snapshot length"); break }
            // Output failures propagate: a broken ZIP must never be returned as a successful export.
            zip.write(buffer, 0, count)
            included += count
        }
        zip.closeEntry()
        record.put("included_bytes", included).put("status", when {
            readError != null -> "partial_read_error"
            start > 0 || end < size -> "truncated"
            else -> "included"
        })
        if (readError != null) record.put("error", safeError(readError))
        return record
    }

    private fun readSessionState(session : File, active : Boolean) : JSONObject = try {
        val file = File(session, "session.json")
        if (file.length() > MIB) throw IOException("Session metadata exceeds size limit")
        val metadata = JSONObject(file.readText())
        JSONObject().put("recorded_status", metadata.optString("status", "unknown"))
            .put("finish_reason", metadata.opt("finish_reason") ?: JSONObject.NULL)
            .put("interrupted_without_finish", !active && metadata.optString("status") == "running")
            .put("note", "An unlocked running session ended without finish; this can be a native crash, process kill or interrupted startup, not a proven cause")
    } catch (e : Exception) {
        unavailable(safeError(e))
    }

    private fun safeError(error : Exception) = error.javaClass.simpleName + ": " + error.message.orEmpty().take(512)

    private class LimitedOutput(private val target : OutputStream, private val limit : Long) : OutputStream() {
        private var count = 0L

        override fun write(value : Int) {
            ensureCapacity(1)
            target.write(value)
            count++
        }

        override fun write(buffer : ByteArray, offset : Int, length : Int) {
            ensureCapacity(length)
            target.write(buffer, offset, length)
            count += length
        }

        private fun ensureCapacity(length : Int) {
            if (length > limit - count) throw IOException("Diagnostic ZIP exceeded its 64 MiB limit")
        }

        override fun flush() = target.flush()
        override fun close() = target.close()
    }
}
