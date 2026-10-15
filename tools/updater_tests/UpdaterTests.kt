// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.updater

import com.sun.net.httpserver.HttpServer
import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import java.net.InetSocketAddress
import java.nio.file.Files
import java.util.concurrent.CancellationException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

private var checks = 0
private fun test(name: String, action: () -> Unit) { action(); checks++; println("PASS: $name") }
private fun fails(action: () -> Unit) {
    var failed = false
    try { action() } catch (_: IOException) { failed = true }
    check(failed) { "Expected an I/O failure" }
}
private fun zip(path: File, entries: List<Pair<String, ByteArray>>) {
    ZipOutputStream(path.outputStream()).use { out ->
        for ((name, bytes) in entries) { out.putNextEntry(ZipEntry(name)); out.write(bytes); out.closeEntry() }
    }
}

fun main() {
    val dir = Files.createTempDirectory("strato-updater-tests").toFile()
    val data = ByteArray(200_000) { (it % 251).toByte() }
    val out = File(dir, "update.apk")
    val archive = File(dir, "update.zip")
    val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
    val executor = Executors.newCachedThreadPool()
    server.executor = executor
    val started = CountDownLatch(1)
    val release = CountDownLatch(1)
    server.createContext("/known") { exchange ->
        exchange.sendResponseHeaders(200, data.size.toLong())
        exchange.responseBody.use { it.write(data) }
    }
    server.createContext("/unknown") { exchange ->
        exchange.sendResponseHeaders(200, 0)
        exchange.responseBody.use { it.write(data) }
    }
    server.createContext("/redirect") { exchange ->
        exchange.responseHeaders.set("Location", "/known")
        exchange.sendResponseHeaders(302, -1); exchange.close()
    }
    server.createContext("/error") { exchange -> exchange.sendResponseHeaders(503, -1); exchange.close() }
    server.createContext("/empty") { exchange -> exchange.sendResponseHeaders(200, -1); exchange.close() }
    server.createContext("/slow") { exchange ->
        try {
            exchange.sendResponseHeaders(200, 500_000)
            exchange.responseBody.write(data)
            exchange.responseBody.flush()
            started.countDown()
            release.await(5, TimeUnit.SECONDS)
        } finally { exchange.close() }
    }
    server.start()
    val base = "http://127.0.0.1:${server.address.port}"
    try {
        test("known-length HTTP transfer and monotonic progress") {
            val progress = mutableListOf<Long>()
            APKDownloader().download("$base/known", out, {}) { bytes, total ->
                check(total == data.size.toLong()); progress += bytes
            }
            check(out.readBytes().contentEquals(data))
            check(progress.first() == 0L && progress.last() == data.size.toLong())
            check(progress.zipWithNext().all { it.first <= it.second })
        }
        test("unknown HTTP length is accepted without fake percentage") {
            out.delete()
            APKDownloader().download("$base/unknown", out, {}) { _, total -> check(total == -1L) }
            check(out.readBytes().contentEquals(data))
        }
        test("HTTP redirect followed") {
            out.delete(); APKDownloader().download("$base/redirect", out, {}) { _, _ -> }
            check(out.readBytes().contentEquals(data))
        }
        test("HTTP error preserves complete APK and removes stale partial") {
            File(out.path + ".part").writeText("interrupted previous process")
            fails { APKDownloader().download("$base/error", out, {}) { _, _ -> } }
            check(out.readBytes().contentEquals(data))
            check(!File(out.path + ".part").exists())
        }
        test("empty download rejected and partial removed") {
            out.delete(); fails { APKDownloader().download("$base/empty", out, {}) { _, _ -> } }
            check(!out.exists() && !File(out.path + ".part").exists())
        }
        test("truncated and oversized streams rejected") {
            for (size in listOf(data.size.toLong() + 1, data.size.toLong() - 1)) {
                fails { UpdateFiles.copy(ByteArrayInputStream(data), out, size, {}, progress = { _, _ -> }) }
                check(!out.exists() && !File(out.path + ".part").exists())
            }
        }
        test("size limit checked before reading") {
            fails { UpdateFiles.copy(ByteArrayInputStream(data), out, UpdateFiles.MAX_BYTES + 1, {}, progress = { _, _ -> }) }
            check(!out.exists())
        }
        test("cancellation during copy removes partial but preserves prior completed file") {
            out.writeText("previous complete file")
            var reads = 0
            var cancelled = false
            try {
                UpdateFiles.copy(ByteArrayInputStream(data), out, data.size.toLong(), {
                    if (++reads > 2) throw CancellationException()
                }, progress = { _, _ -> })
            } catch (_: CancellationException) { cancelled = true }
            check(cancelled && out.readText() == "previous complete file")
            check(!File(out.path + ".part").exists())
        }
        test("ZIP single nested APK extracted once to fixed path") {
            out.delete(); zip(archive, listOf("notes.txt" to byteArrayOf(1), "nested/app.APK" to data))
            UpdateFiles.extractApk(archive, out, {}) { _, total -> check(total == data.size.toLong()) }
            check(out.readBytes().contentEquals(data))
            check(!File(dir, "nested").exists())
        }
        test("ZIP without APK and ZIP with multiple APKs fail explicitly") {
            out.delete()
            for (entries in listOf(listOf("notes.txt" to data), listOf("a.apk" to data, "b.apk" to data))) {
                zip(archive, entries)
                fails { UpdateFiles.extractApk(archive, out, {}) { _, _ -> } }
                check(!out.exists())
            }
        }
        test("damaged ZIP rejected") {
            archive.writeText("not a ZIP")
            fails { UpdateFiles.extractApk(archive, out, {}) { _, _ -> } }
            check(!out.exists())
        }
        test("corrupt APK CRC rejected and partial removed") {
            zip(archive, listOf("app.apk" to data))
            val bytes = archive.readBytes()
            val offset = (0 until bytes.size - 4).first {
                bytes[it] == 0x50.toByte() && bytes[it+1] == 0x4b.toByte() &&
                    bytes[it+2] == 1.toByte() && bytes[it+3] == 2.toByte()
            }
            bytes[offset + 16] = (bytes[offset + 16].toInt() xor 0xff).toByte()
            archive.writeBytes(bytes)
            fails { UpdateFiles.extractApk(archive, out, {}) { _, _ -> } }
            check(!out.exists() && !File(out.path + ".part").exists())
        }
        test("pre-cancelled downloader never starts transfer") {
            val downloader = APKDownloader(); downloader.cancel()
            var cancelled = false
            try { downloader.download("$base/known", out, {}) { _, _ -> } }
            catch (_: CancellationException) { cancelled = true }
            check(cancelled && !out.exists())
        }
        test("cancelling blocked network read returns promptly and cleans partial") {
            val downloader = APKDownloader()
            val failure = AtomicReference<Throwable?>()
            val worker = Thread {
                try { downloader.download("$base/slow", out, {}) { _, _ -> } }
                catch (e: Throwable) { failure.set(e) }
            }
            worker.start(); check(started.await(5, TimeUnit.SECONDS))
            val before = System.nanoTime(); downloader.cancel()
            check(System.nanoTime() - before < 500_000_000L)
            release.countDown(); worker.join(5000)
            check(!worker.isAlive && failure.get() != null)
            check(!out.exists() && !File(out.path + ".part").exists())
        }
        println("$checks test groups passed; real downloader/extractor, local HTTP, no Android installation executed.")
    } finally {
        release.countDown(); server.stop(0); executor.shutdownNow(); dir.deleteRecursively()
    }
}
