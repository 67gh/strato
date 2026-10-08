/*
 * SPDX-License-Identifier: MPL-2.0
 */

package org.stratoemu.strato.diagnostics

import android.app.ActivityManager
import android.content.Context
import android.os.Build
import android.os.Process
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.util.AtomicFile
import androidx.preference.PreferenceManager
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.json.JSONArray
import org.json.JSONObject
import org.stratoemu.strato.BuildConfig
import org.stratoemu.strato.settings.EmulationSettings
import org.stratoemu.strato.settings.NativeSettings
import java.io.Closeable
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.channels.FileLock
import java.nio.channels.OverlappingFileLockException
import java.nio.file.Files
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * One launch, in the emulation process. Native writers must use [directory] directly;
 * copying global logs here would silently attribute an earlier game's failure to this one.
 * A held OS file lock distinguishes a live session from a process killed without cleanup.
 */
class DiagnosticSession private constructor(
    private val context : Context,
    val directory : File,
    private val sessionLock : DiagnosticFileLock,
    private val sessionInfo : JSONObject
) {
    private data class Performance(val fps : Double, val frametimeMs : Float, val sampledAtMs : Long)

    private val finished = AtomicBoolean(false)
    private val performance = AtomicReference<Performance?>(null)
    private val stateLock = Any()
    private val issues = linkedMapOf<String, JSONObject>()
    private val timeline = DiagnosticRollingLog(directory, "timeline.jsonl", "timeline.previous.jsonl", MIB)
    private val logcat = DiagnosticRollingLog(directory, "logcat.log", "logcat.previous.log", 4 * MIB)
    private val scheduler = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "diagnostic-timeline").apply { isDaemon = true }
    }
    private val logcatTimeouts = Executors.newSingleThreadScheduledExecutor { runnable ->
        Thread(runnable, "diagnostic-logcat-timeout").apply { isDaemon = true }
    }
    @Volatile private var logcatProcess : java.lang.Process? = null
    private var logcatThread : Thread? = null
    private var logcatState = "not_started"
    private var logcatExitCode : Int? = null
    private var timelineState = "not_started"
    private var previousCpuMs = Process.getElapsedCpuTime()
    private var previousElapsedMs = SystemClock.elapsedRealtime()
    private val startedElapsedMs = previousElapsedMs
    private val pageSizeBytes = try {
        Os.sysconf(OsConstants._SC_PAGESIZE).takeIf { it > 0 }
    } catch (_ : Exception) {
        null
    }
    private val processors = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)

    /** Called by the activity's periodic statistics update, including when its overlay is hidden. */
    fun updatePerformance(fps : Double, averageFrametimeMs : Float) {
        if (!finished.get() && fps.isFinite() && fps >= 0 && averageFrametimeMs.isFinite() && averageFrametimeMs >= 0)
            performance.set(Performance(fps, averageFrametimeMs, SystemClock.elapsedRealtime()))
    }

    /** Best effort only: this must never replace or mask the original uncaught exception. */
    fun recordCrash(throwable : Throwable) {
        try {
            synchronized(stateLock) {
                if (finished.get()) return
                val text = buildString {
                    append("UTC: ").append(utcNow()).append("\nPID: ").append(Process.myPid()).append('\n')
                    append("Thread: ").append(Thread.currentThread().name.take(256)).append('\n')
                    var cause : Throwable? = throwable
                    val seen = java.util.Collections.newSetFromMap(java.util.IdentityHashMap<Throwable, Boolean>())
                    var count = 0
                    while (count++ < 8 && length < 60 * 1024) {
                        val currentCause = cause ?: break
                        if (!seen.add(currentCause)) break
                        append(currentCause.javaClass.name).append(": ").append(currentCause.message?.take(2048)).append('\n')
                        currentCause.stackTrace.take(128).forEach { append("  at ").append(it.toString().take(512)).append('\n') }
                        if (currentCause.suppressed.isNotEmpty()) append("Suppressed exceptions: ").append(currentCause.suppressed.size).append('\n')
                        cause = currentCause.cause
                    }
                    append("Trace limited to 8 causes and 128 frames per cause.\n")
                }.take(64 * 1024)
                // Only this session is written; the application's existing crash log remains intact.
                writeDiagnosticBytes(File(directory, "crash.log"), text.toByteArray(Charsets.UTF_8))
                sessionInfo.put("uncaught_exception_recorded", true)
                persistSession()
            }
        } catch (_ : Throwable) {
        }
    }

    /** "native_returned" means execution returned, not that the guest finished successfully. */
    fun finish(reason : String) {
        if (!finished.compareAndSet(false, true)) return
        try {
            stopCollectors()
            synchronized(stateLock) {
                sessionInfo.put("status", "finished")
                    .put("finish_reason", reason.take(256))
                    .put("ended_at_utc", utcNow())
                    .put("ended_at_epoch_ms", System.currentTimeMillis())
                persistSession()
            }
            persistCollectorStatus(force = true)
        } catch (_ : Throwable) {
            // Cleanup must not replace an exception propagated from native execution.
        } finally {
            try { timeline.close() } catch (_ : Throwable) { }
            try { logcat.close() } catch (_ : Throwable) { }
            try { sessionLock.close() } catch (_ : Throwable) { }
            synchronized(CURRENT_LOCK) {
                if (current === this) current = null
            }
        }
    }

    private fun initialize(titleId : String?, nativeSettings : NativeSettings) {
        // These snapshots are persisted before JNI starts, even if no native initialization succeeds.
        writeDiagnosticJson(File(directory, "session.json"), sessionInfo)
        writeDiagnosticJson(File(directory, "device.json"), deviceSnapshot())
        writeDiagnosticJson(File(directory, "settings.json"), settingsSnapshot(titleId, nativeSettings))
        startLogcat()
        synchronized(stateLock) { timelineState = "running" }
        persistCollectorStatus()
        scheduler.scheduleAtFixedRate({
            if (!finished.get()) {
                try {
                    sampleTimeline()
                } catch (e : Exception) {
                    issue("timeline_sample", e)
                }
                persistCollectorStatus()
            }
        }, 0, 1, TimeUnit.SECONDS)
    }

    private fun deviceSnapshot() = JSONObject().apply {
        put("schema_version", 1)
        put("captured_at_utc", utcNow())
        put("manufacturer", Build.MANUFACTURER)
        put("model", Build.MODEL)
        put("device", Build.DEVICE)
        put("board", Build.BOARD)
        put("hardware", Build.HARDWARE)
        put("supported_abis", JSONArray(Build.SUPPORTED_ABIS.toList()))
        put("android_release", Build.VERSION.RELEASE)
        put("android_sdk", Build.VERSION.SDK_INT)
        put("android_security_patch", Build.VERSION.SECURITY_PATCH)
        put("soc", if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            JSONObject().put("manufacturer", Build.SOC_MANUFACTURER).put("model", Build.SOC_MODEL).put("source", "android.os.Build")
        } else {
            unavailable("Build.SOC_MODEL requires Android 12; hardware/board are reported separately and are not an exact SoC identification")
        })
        put("available_processors", processors)
        put("page_size_bytes", pageSizeBytes ?: JSONObject.NULL)
        put("memory", capture("device_memory") { memorySnapshot() })
        put("app", JSONObject().put("package", context.packageName)
            .put("version_name", BuildConfig.VERSION_NAME).put("version_code", BuildConfig.VERSION_CODE)
            .put("build_type", BuildConfig.BUILD_TYPE).put("commit", BuildConfig.BUILD_COMMIT_FULL)
            .put("build_timestamp_seconds", BuildConfig.BUILD_TIMESTAMP))
        put("firmware_version", PreferenceManager.getDefaultSharedPreferences(context).getString("firmware", null) ?: JSONObject.NULL)
        put("vulkan", JSONObject().put("source", "vulkan.json").put("status", "see_native_capture"))
        put("jit_compiled", JSONObject().put("source", "native.json").put("status", "see_native_capture"))
    }

    private fun memorySnapshot() : JSONObject {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            ?: throw IOException("ActivityManager unavailable")
        val info = ActivityManager.MemoryInfo()
        manager.getMemoryInfo(info)
        return JSONObject().put("total_bytes", info.totalMem).put("available_bytes", info.availMem)
            .put("low_memory", info.lowMemory).put("low_memory_threshold_bytes", info.threshold)
            .put("available_note", "Android available memory, not an estimate of GPU memory")
    }

    private fun settingsSnapshot(titleId : String?, effective : NativeSettings) = JSONObject().apply {
        put("schema_version", 1)
        put("captured_at_utc", utcNow())
        put("scope", "launch_snapshot; later settings changes are not tracked in this delivery")
        put("redacted_fields", JSONArray(listOf("usernameValue", "profilePictureValue")))
        put("global", capture("global_settings") { emulationSnapshot(EmulationSettings.global) })
        put("game", if (titleId != null) capture("game_settings") { emulationSnapshot(EmulationSettings.forTitleId(titleId)) }
            else unavailable("No title preference identifier was supplied"))
        put("effective_native", sanitizedNativeSettings(effective))
        put("effective_ui", capture("effective_ui_settings") {
            uiSettings(if (titleId != null) EmulationSettings.forEmulation(titleId) else EmulationSettings.global)
        })
    }

    private fun emulationSnapshot(settings : EmulationSettings) = JSONObject()
        .put("is_global", settings.isGlobal).put("custom_settings_enabled", settings.useCustomSettings)
        .put("native", sanitizedNativeSettings(NativeSettings(context, settings))).put("ui", uiSettings(settings))

    private fun uiSettings(settings : EmulationSettings) = JSONObject()
        .put("perf_stats", settings.perfStats).put("max_refresh_rate", settings.maxRefreshRate)
        .put("orientation", settings.orientation).put("aspect_ratio", settings.aspectRatio)
        .put("respect_display_cutout", settings.respectDisplayCutout).put("enable_foldable_layout", settings.enableFoldableLayout)
        .put("show_pause_button", settings.showPauseButton)

    private fun sanitizedNativeSettings(settings : NativeSettings) = JSONObject(Json.encodeToString(settings)).apply {
        put("usernameValue", "[redacted]")
        put("profilePictureValue", "[redacted]")
    }

    private fun capture(name : String, block : () -> JSONObject) : JSONObject = try {
        block()
    } catch (e : Exception) {
        issue(name, e)
        unavailable(e.javaClass.simpleName + ": " + e.message.orEmpty().take(512))
    }

    private fun sampleTimeline() {
        val now = SystemClock.elapsedRealtime()
        val cpu = Process.getElapsedCpuTime()
        val interval = now - previousElapsedMs
        val cpuDelta = cpu - previousCpuMs
        val stats = performance.get()
        val statsAge = stats?.let { now - it.sampledAtMs }
        val freshStats = stats?.takeIf { statsAge != null && statsAge in 0L..2500L }
        val sample = JSONObject().put("utc", utcNow()).put("elapsed_ms", now - startedElapsedMs)
            .put("pid", Process.myPid()).put("sample_interval_ms", interval)
            .put("cpu_percent_one_core", if (interval > 0 && cpuDelta >= 0) 100.0 * cpuDelta / interval else JSONObject.NULL)
            .put("cpu_percent_available_cores", if (interval > 0 && cpuDelta >= 0) 100.0 * cpuDelta / interval / processors else JSONObject.NULL)
            .put("available_processors", processors)
            .put("fps", freshStats?.fps ?: JSONObject.NULL)
            .put("average_frametime_ms", freshStats?.takeIf { it.fps > 0 }?.frametimeMs ?: JSONObject.NULL)
            .put("fps_sample_age_ms", statsAge ?: JSONObject.NULL)
            .put("fps_status", if (freshStats != null) "available" else if (stats == null) "not_sampled" else "stale")
        previousElapsedMs = now
        previousCpuMs = cpu
        try {
            val rss = if (pageSizeBytes != null) {
                val fields = File("/proc/self/statm").readText().trim().split(Regex("\\s+"))
                fields[1].toLong() * pageSizeBytes
            } else {
                val rssLine = File("/proc/self/status").useLines { lines -> lines.firstOrNull { it.startsWith("VmRSS:") } }
                    ?: throw IOException("VmRSS is unavailable and page size could not be read")
                rssLine.substringAfter(':').trim().substringBefore(' ').toLong() * 1024L
            }
            sample.put("rss_bytes", rss).put("rss_source", if (pageSizeBytes != null) "statm_resident_times_sysconf_pagesize" else "status_VmRSS_kib")
        } catch (e : Exception) {
            sample.put("rss_bytes", JSONObject.NULL).put("rss_error", e.javaClass.simpleName)
            issue("rss", e)
        }
        try {
            sample.put("host_thread_count", File("/proc/self/task").list()?.size ?: throw IOException("Cannot list process threads"))
        } catch (e : Exception) {
            sample.put("host_thread_count", JSONObject.NULL)
            issue("host_threads", e)
        }
        // Host threads are not guest threads. Native GPU/IPC counters are deliberately not fabricated.
        sample.put("guest_thread_count", JSONObject.NULL).put("gpu_allocated_bytes", JSONObject.NULL)
            .put("draws", JSONObject.NULL).put("dispatches", JSONObject.NULL).put("pipelines_compiled", JSONObject.NULL)
        timeline.append(sample.toString())
    }

    private fun startLogcat() {
        logcatThread = Thread({
            // A finite -d command also exits on its own if our parent process is killed. A
            // persistent follower could be orphaned, waiting forever for logs from a dead PID.
            var cursor = sessionInfo.getLong("started_at_epoch_ms") / 1000.0
            var cursorCounts = mutableMapOf<String, Int>()
            val digest = MessageDigest.getInstance("SHA-256")
            while (!finished.get()) {
                val pollStartedAt = SystemClock.elapsedRealtime()
                try {
                    val cutoff = cursor
                    val previousCounts = cursorCounts.toMap()
                    val seenAtCutoff = mutableMapOf<String, Int>()
                    val newestCounts = cursorCounts.toMutableMap()
                    cursorCounts = newestCounts
                    val since = SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(Date((cursor * 1000).toLong()))
                    val child = ProcessBuilder("/system/bin/logcat", "-d", "--pid=${Process.myPid()}",
                        "-b", "main", "-b", "system", "-b", "crash", "-v", "threadtime", "-v", "epoch", "-v", "usec", "-T", since)
                        .redirectErrorStream(true).start()
                    logcatProcess = child
                    synchronized(stateLock) { logcatState = "polling" }
                    val timedOut = AtomicBoolean(false)
                    var timeout : ScheduledFuture<*>? = null
                    try {
                        if (finished.get()) break
                        timeout = logcatTimeouts.schedule(Runnable {
                            timedOut.set(true)
                            try { child.destroyForcibly() } catch (_ : Exception) { }
                        }, 2, TimeUnit.SECONDS)
                        readLogcatLines(child) { line ->
                            val epochSeconds = line.trimStart().substringBefore(' ').toDoubleOrNull()
                            if (epochSeconds != null && epochSeconds.isFinite() && epochSeconds >= cutoff) {
                                val identity = digest.digest(line.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
                                var include = true
                                if (epochSeconds == cutoff) {
                                    val occurrence = (seenAtCutoff[identity] ?: 0) + 1
                                    if (seenAtCutoff.size < 4096 || identity in seenAtCutoff) seenAtCutoff[identity] = occurrence
                                    include = occurrence > (previousCounts[identity] ?: 0)
                                    if (cursor == cutoff && (newestCounts.size < 4096 || identity in newestCounts))
                                        newestCounts[identity] = maxOf(occurrence, newestCounts[identity] ?: 0)
                                } else {
                                    if (epochSeconds > cursor) { cursor = epochSeconds; newestCounts.clear() }
                                    if (epochSeconds == cursor && (newestCounts.size < 4096 || identity in newestCounts))
                                        newestCounts[identity] = (newestCounts[identity] ?: 0) + 1
                                }
                                if (newestCounts.size >= 4096) issue("logcat_dedup_capacity", IOException("Timestamp deduplication reached its 4096-record bound; duplicates may remain"))
                                if (include && !finished.get()) logcat.append(line)
                            } else if (line.isNotBlank() && !line.startsWith("---------")) {
                                if (epochSeconds == null) issue("logcat_output", IOException(line.take(512)))
                            }
                        }
                        if (!child.waitFor(200, TimeUnit.MILLISECONDS)) child.destroyForcibly()
                        synchronized(stateLock) { logcatExitCode = if (child.isAlive) null else child.exitValue() }
                        if (timedOut.get()) {
                            synchronized(stateLock) { logcatState = "incomplete" }
                            issue("logcat_timeout", IOException("A logcat snapshot exceeded 2 seconds; it may be incomplete"))
                        } else if (!finished.get() && logcatExitCode != 0) {
                            synchronized(stateLock) { logcatState = "failed" }
                            issue("logcat_exit", IOException("Logcat snapshot exit code $logcatExitCode"))
                        } else synchronized(stateLock) { logcatState = "available" }
                    } finally {
                        timeout?.cancel(false)
                        try { if (child.isAlive) child.destroyForcibly() } catch (_ : Exception) { }
                        try { child.inputStream.close() } catch (_ : Exception) { }
                        logcatProcess = null
                    }
                } catch (e : Exception) {
                    if (!finished.get()) {
                        synchronized(stateLock) { logcatState = "failed" }
                        issue("logcat_snapshot", e)
                    }
                } finally {
                    persistCollectorStatus()
                }
                val delayMs = (2000 - (SystemClock.elapsedRealtime() - pollStartedAt)).coerceAtLeast(0)
                try { Thread.sleep(delayMs) } catch (_ : InterruptedException) { break }
            }
        }, "diagnostic-logcat").apply { isDaemon = true; start() }
    }

    private fun readLogcatLines(child : java.lang.Process, consume : (String) -> Unit) {
        val buffer = ByteArray(8192)
        val line = ByteArrayOutputStream(4096)
        var oversize = false
        var total = 0L
        child.inputStream.buffered().use { input ->
            while (!finished.get() && total < 4 * MIB) {
                val count = input.read(buffer, 0, minOf(buffer.size.toLong(), 4 * MIB - total).toInt())
                if (count < 0) return
                total += count
                var segmentStart = 0
                for (index in 0..count) {
                    if (index == count || buffer[index] == '\n'.code.toByte()) {
                        if (!oversize && line.size() + index - segmentStart <= 64 * 1024) line.write(buffer, segmentStart, index - segmentStart)
                        else oversize = true
                        if (index < count) {
                            if (oversize) issue("logcat_line_size", IOException("A line over 64 KiB was omitted"))
                            else consume(line.toString("UTF-8"))
                            line.reset()
                            oversize = false
                        }
                        segmentStart = index + 1
                    }
                }
            }
        }
        if (total >= 4 * MIB) issue("logcat_poll_size", IOException("A snapshot reached its 4 MiB read limit; unread buffer entries may be lost"))
    }

    private fun issue(key : String, error : Exception) {
        synchronized(stateLock) {
            // Coalesce errors instead of repeating an inaccessible counter once per second.
            val old = issues[key]
            if (old != null) {
                old.put("count", old.optLong("count") + 1)
            } else if (issues.size < 64) {
                issues[key] = JSONObject()
                    .put("component", key)
                    .put("count", 1)
                    .put("first_seen_utc", utcNow())
                    .put("error", error.javaClass.simpleName + ": " + error.message.orEmpty().take(512))
            }
        }
    }

    private fun persistSession() {
        try {
            writeDiagnosticJson(File(directory, "session.json"), sessionInfo)
        } catch (e : Exception) {
            issue("session_write", e)
        }
    }

    private fun persistCollectorStatus(force : Boolean = false) {
        try {
            synchronized(stateLock) {
                if (finished.get() && !force) return
                writeDiagnosticJson(File(directory, "collector.json"), JSONObject().put("schema_version", 1)
                    .put("updated_at_utc", utcNow()).put("pid", Process.myPid())
                    .put("logcat", logcat.snapshot().put("status", logcatState).put("exit_code", logcatExitCode ?: JSONObject.NULL)
                        .put("scope", "current emulation PID, entries since session start; Android may restrict buffers")
                        .put("poll_interval_ms", 2000).put("poll_timeout_ms", 2000).put("poll_read_limit_bytes", 4 * MIB)
                        .put("polling_limitations", "The final interval may be absent after abrupt process death; Android buffer overwrite or wall-clock changes can lose records"))
                    .put("timeline", timeline.snapshot().put("status", timelineState).put("interval_ms", 1000)
                        .put("fps_source", "real frame present submissions per callback interval; not physical display/GPU timing")
                        .put("average_frametime_ms", "callback interval divided by real frame submissions; unavailable when fps is zero"))
                    .put("errors", JSONArray(issues.values.toList()))
                    .put("cpu_definition", "100% one_core = one logical CPU fully busy; available_cores divides by Runtime.availableProcessors at launch")
                    .put("unavailable_in_this_delivery", JSONArray(listOf("gpu_utilization_percent", "gpu_allocation_timeline", "draw_dispatch_pipeline_counters", "guest_thread_timeline", "ipc_command_ring"))))
            }
        } catch (e : Exception) {
            issue("collector_status_write", e)
        }
    }

    private fun stopCollectors() {
        try { scheduler.shutdownNow() } catch (_ : Throwable) { }
        try { logcatTimeouts.shutdownNow() } catch (_ : Throwable) { }
        try { logcatThread?.interrupt() } catch (_ : Throwable) { }
        try { logcatProcess?.destroyForcibly() } catch (_ : Throwable) { }
        try { logcatProcess?.inputStream?.close() } catch (_ : Throwable) { }
        try { scheduler.awaitTermination(500, TimeUnit.MILLISECONDS) } catch (_ : InterruptedException) { Thread.currentThread().interrupt() }
        try { logcatThread?.join(500) } catch (_ : InterruptedException) { Thread.currentThread().interrupt() }
        try { timeline.close() } catch (_ : Throwable) { }
        try { logcat.close() } catch (_ : Throwable) { }
        synchronized(stateLock) {
            timelineState = "stopped"
            if (logcatState == "polling" || logcatState == "available") logcatState = "stopped"
        }
    }

    private fun rollback(error : Exception) {
        finished.set(true)
        try {
            stopCollectors()
            synchronized(stateLock) {
                sessionInfo.put("status", "collector_start_failed").put("finish_reason", error.javaClass.simpleName)
                persistSession()
            }
        } catch (_ : Throwable) {
        } finally {
            try { timeline.close() } catch (_ : Throwable) { }
            try { logcat.close() } catch (_ : Throwable) { }
            try { sessionLock.close() } catch (_ : Throwable) { }
        }
    }

    companion object {
        private const val MIB = 1024L * 1024L
        private val CURRENT_LOCK = Any()
        @Volatile private var current : DiagnosticSession? = null

        @JvmStatic
        fun start(context : Context, titleId : String?, gameName : String, nativeSettings : NativeSettings) : DiagnosticSession = synchronized(CURRENT_LOCK) {
            if (current?.finished?.get() == false) throw IOException("A diagnostic session is already active in this process")
            val appContext = context.applicationContext
            val root = sessionRoot(appContext)
            if (!root.isDirectory && !root.mkdirs()) throw IOException("Cannot create diagnostic sessions directory")
            val startTime = System.currentTimeMillis()
            val directory = File(root, "$startTime-${Process.myPid()}-${UUID.randomUUID().toString().take(8)}")
            if (!directory.mkdir()) throw IOException("Cannot create diagnostic session")
            val lock = DiagnosticFileLock.acquire(directory) ?: throw IOException("Cannot lock new diagnostic session")
            val metadata = JSONObject().put("schema_version", 1).put("session_id", directory.name)
                .put("game_name", gameName.take(512)).put("title_id", titleId?.take(256) ?: JSONObject.NULL)
                .put("started_at_utc", utcTime(startTime)).put("started_at_epoch_ms", startTime)
                .put("pid", Process.myPid()).put("status", "running").put("finish_reason", JSONObject.NULL)
                .put("ended_at_utc", JSONObject.NULL)
            val session = try {
                DiagnosticSession(appContext, directory, lock, metadata)
            } catch (e : Exception) {
                lock.close()
                throw e
            }
            try {
                session.initialize(titleId, nativeSettings)
                current = session
                session
            } catch (e : Exception) {
                session.rollback(e)
                throw e
            }
        }

        @JvmStatic
        fun listSessions(context : Context) : List<File> {
            val root = sessionRoot(context).canonicalFile
            return root.listFiles()?.filter {
                try { !Files.isSymbolicLink(it.toPath()) && it.isDirectory && it.canonicalFile.parentFile == root && File(it, "session.json").isFile }
                catch (_ : IOException) { false }
            }?.sortedByDescending { it.name } ?: emptyList()
        }

        /** On an inspection error, conservatively treat the session as active. No sessions are pruned here. */
        @JvmStatic
        fun isSessionActive(directory : File) : Boolean = try {
            val lock = DiagnosticFileLock.acquire(directory)
            if (lock == null) true else { lock.close(); false }
        } catch (_ : Exception) {
            true
        }

        @JvmStatic
        fun recordCurrentCrash(throwable : Throwable) {
            try { current?.recordCrash(throwable) } catch (_ : Throwable) { }
        }

        internal fun sessionRoot(context : Context) = File(context.filesDir, "diagnostics/sessions")
    }
}

internal fun utcTime(time : Long) = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US).apply {
    timeZone = TimeZone.getTimeZone("UTC")
}.format(Date(time))

internal fun utcNow() = utcTime(System.currentTimeMillis())

internal fun unavailable(reason : String) = JSONObject().put("status", "unavailable").put("reason", reason)

internal fun writeDiagnosticJson(file : File, value : JSONObject) = writeDiagnosticBytes(file, value.toString(2).toByteArray(Charsets.UTF_8))

internal fun writeDiagnosticBytes(file : File, bytes : ByteArray) {
    val atomic = AtomicFile(file)
    val output = atomic.startWrite()
    try {
        output.write(bytes)
        atomic.finishWrite(output)
    } catch (e : Exception) {
        atomic.failWrite(output)
        throw e
    }
}

/** Each lock is released automatically by the kernel if its owning process dies. */
internal class DiagnosticFileLock private constructor(private val path : String, private val file : RandomAccessFile, private val lock : FileLock) : Closeable {
    private val closed = AtomicBoolean(false)

    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        synchronized(HELD) {
            try { lock.release() } catch (_ : Exception) { }
            try { file.close() } catch (_ : Exception) { }
            if (HELD[path] === this) HELD.remove(path)
        }
    }

    companion object {
        private val HELD = mutableMapOf<String, DiagnosticFileLock>()

        fun acquire(directory : File) : DiagnosticFileLock? = synchronized(HELD) {
            val lockFile = File(directory, ".active.lock")
            if (Files.isSymbolicLink(lockFile.toPath())) throw IOException("A session lock must not be a symbolic link")
            val path = lockFile.canonicalPath
            // POSIX locks can be dropped when another descriptor for the same file is closed.
            // Do not even open a second descriptor for locks already held in this JVM.
            if (path in HELD) return@synchronized null
            val file = RandomAccessFile(path, "rw")
            try {
                val lock = try { file.channel.tryLock() } catch (_ : OverlappingFileLockException) { null }
                if (lock == null) { file.close(); return@synchronized null }
                DiagnosticFileLock(path, file, lock).also { HELD[path] = it }
            } catch (e : Exception) {
                file.close()
                throw e
            }
        }
    }
}

/** Keep the latest two chunks, with complete JSONL records. Never allocate an unbounded log in RAM. */
private class DiagnosticRollingLog(directory : File, name : String, previousName : String, private val chunkLimit : Long) : Closeable {
    private val file = File(directory, name)
    private val previous = File(directory, previousName)
    private var output : FileOutputStream? = null
    private var size = 0L
    private var bytesObserved = 0L
    private var rotations = 0L
    private var oversizeLines = 0L
    private var closed = false

    @Synchronized
    fun append(line : String) {
        if (closed) return
        val bytes = (line + "\n").toByteArray(Charsets.UTF_8)
        bytesObserved += bytes.size
        // No truncated JSON is written; an oversized record is reported as omitted.
        if (bytes.size > 64 * 1024) { oversizeLines++; return }
        if (size + bytes.size > chunkLimit) {
            output?.close()
            output = null
            if (previous.exists() && !previous.delete()) throw IOException("Cannot rotate ${file.name}")
            if (file.exists() && !file.renameTo(previous)) throw IOException("Cannot rotate ${file.name}")
            size = 0
            rotations++
        }
        if (output == null) output = FileOutputStream(file, true)
        output!!.write(bytes)
        size += bytes.size
    }

    @Synchronized
    fun snapshot() = JSONObject().put("chunk_limit_bytes", chunkLimit).put("retention_limit_bytes", chunkLimit * 2)
        .put("bytes_observed", bytesObserved).put("retained_bytes", file.length() + previous.length())
        .put("rotations", rotations).put("oversize_records_omitted", oversizeLines)
        .put("retention", "latest current and previous chunks; older chunks are discarded")

    @Synchronized
    override fun close() {
        closed = true
        try { output?.close() } catch (_ : IOException) { }
        output = null
    }
}
