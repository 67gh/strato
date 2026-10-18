// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.updater

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.File

data class InstallEvent(val id: Int, val url: String, val status: Int,
                        val detail: String = "", val confirmation: Intent? = null)

/** Owns one update session; no Activity reference and no background UI launches. */
object UpdateSession {
    const val STAGING = -100
    const val COMMITTED = -101
    private const val ACTION = "org.stratoemu.strato.UPDATE_INSTALL_RESULT"
    private val mutable = MutableStateFlow<InstallEvent?>(null)
    val events = mutable.asStateFlow()
    private var consumedConfirmation: Intent? = null
    private fun prefs(context: Context) = context.getSharedPreferences("update_install_result", Context.MODE_PRIVATE)

    @Synchronized
    fun restore(context: Context): InstallEvent? {
        if (mutable.value != null) return mutable.value
        val p = prefs(context)
        if (!p.contains("id")) return null
        // A pending confirmation Intent is process-local. After process death the
        // user can retry, which abandons that session before creating another.
        val status = p.getInt("status", COMMITTED)
        val event = InstallEvent(p.getInt("id", -1), p.getString("url", "").orEmpty(),
            if (status == STAGING) PackageInstaller.STATUS_FAILURE else status,
            if (status == STAGING) "Installation preparation interrupted" else p.getString("detail", "").orEmpty())
        mutable.value = event
        return event
    }

    @Synchronized
    private fun publish(context: Context, event: InstallEvent) {
        prefs(context).edit().putInt("id", event.id).putString("url", event.url)
            .putInt("status", event.status).putString("detail", event.detail).apply()
        mutable.value = event
    }

    /** Call on Dispatchers.IO. Cancellation before commit abandons the staged bytes. */
    fun install(context: Context, apk: File, url: String, check: () -> Unit) {
        val installer = context.packageManager.packageInstaller
        abandon(context)
        check()
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            setSize(apk.length())
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
        }
        val id = installer.createSession(params)
        publish(context, InstallEvent(id, url, STAGING))
        try {
            installer.openSession(id).use { session ->
                session.openWrite("base.apk", 0, apk.length()).use { output ->
                    apk.inputStream().use { input ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            check()
                            val count = input.read(buffer)
                            if (count < 0) break
                            output.write(buffer, 0, count)
                        }
                    }
                    session.fsync(output)
                }
                check()
                val callback = Intent(context, InstallStatusReceiver::class.java).setAction(ACTION)
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                    (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
                val sender = PendingIntent.getBroadcast(context, id, callback, flags).intentSender
                // Publish before commit: a fast callback must not be overwritten.
                publish(context, InstallEvent(id, url, COMMITTED))
                session.commit(sender)
            }
        } catch (e: Exception) {
            runCatching { installer.abandonSession(id) }
            publish(context, InstallEvent(id, url, PackageInstaller.STATUS_FAILURE, e.toString()))
            throw e
        }
    }

    @Suppress("DEPRECATION")
    @Synchronized
    fun receive(context: Context, intent: Intent) {
        if (intent.action != ACTION) return
        val old = restore(context) ?: return
        if (intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1) != old.id) return
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
        val detail = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE).orEmpty()
        val confirmation = if (status == PackageInstaller.STATUS_PENDING_USER_ACTION)
            intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT) else null
        Log.i("StratoUpdater", "Session ${old.id}: status=$status; $detail")
        publish(context, old.copy(status = status, detail = detail, confirmation = confirmation))
    }

    @Synchronized
    fun takeConfirmation(): Intent? {
        val event = mutable.value ?: return null
        val intent = event.confirmation ?: return null
        if (intent === consumedConfirmation) return null
        consumedConfirmation = intent
        return intent
    }

    @Synchronized
    fun abandon(context: Context) {
        val event = restore(context)
        // Clear identity before abandon: its delayed callback must not overwrite a retry.
        prefs(context).edit().clear().apply()
        mutable.value = null
        if (event != null)
            runCatching { context.packageManager.packageInstaller.abandonSession(event.id) }
    }
}
