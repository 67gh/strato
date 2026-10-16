// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.CancellationException
import java.util.concurrent.atomic.AtomicReference

/** One cancellable transfer; callers run it on Dispatchers.IO, never on the UI thread. */
class APKDownloader {
    private val active = AtomicReference<HttpURLConnection?>()
    @Volatile private var cancelled = false

    fun cancel() {
        cancelled = true
        // disconnect() can wait for a socket lock: keep cancellation off the UI thread.
        active.getAndSet(null)?.let { connection ->
            Thread({ connection.disconnect() }, "strato-update-cancel").start()
        }
    }

    fun download(url: String, target: File, checkCancelled: () -> Unit,
                 progress: (Long, Long) -> Unit): File {
        fun check() {
            checkCancelled()
            if (cancelled) throw CancellationException("Update cancelled")
        }
        check()
        val connection = URL(url).openConnection() as HttpURLConnection
        active.set(connection)
        try {
            check()
            connection.connectTimeout = 15_000
            connection.readTimeout = 20_000
            connection.instanceFollowRedirects = true
            connection.useCaches = false
            connection.setRequestProperty("Cache-Control", "no-cache")
            connection.setRequestProperty("Accept-Encoding", "identity")
            connection.setRequestProperty("User-Agent", "Strato-Updater")
            if (connection.responseCode != HttpURLConnection.HTTP_OK)
                throw IOException("Update download HTTP ${connection.responseCode}")
            return connection.inputStream.use {
                UpdateFiles.copy(it, target, connection.contentLengthLong, ::check, progress)
            }
        } finally {
            active.compareAndSet(connection, null)
            connection.disconnect()
            File(target.path + ".part").delete()
        }
    }
}
