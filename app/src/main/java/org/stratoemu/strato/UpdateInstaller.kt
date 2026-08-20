// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project

package org.stratoemu.strato

import android.app.DownloadManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.util.Log
import android.widget.Toast
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.core.net.toUri
import java.io.File

/**
 * @brief Downloads an update APK via the system [DownloadManager] and, once complete, launches
 *        the system package installer to install it - the actual missing piece that made the
 *        previous "check for update" dialog a dead end (it only ever opened a browser tab and
 *        never triggered any download or install on its own).
 *
 * Uses only stock Android APIs (DownloadManager + FileProvider + ACTION_VIEW/ACTION_INSTALL_PACKAGE)
 * rather than any third-party library, since those are the pieces that can be verified against
 * well documented, stable platform behaviour.
 */
object UpdateInstaller {
    private const val TAG = "UpdateInstaller"

    /**
     * @brief Starts downloading [apkUrl] in the background via DownloadManager (shows the
     *        standard system download notification) and automatically prompts the user to
     *        install it once the download finishes.
     */
    fun downloadAndInstall(context : Context, apkUrl : String, displayName : String) {
        val downloadManager = context.getSystemService(Context.DOWNLOAD_SERVICE) as? DownloadManager
        if (downloadManager == null) {
            Toast.makeText(context, context.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
            return
        }

        val destinationDir = File(context.getExternalFilesDir(null), "updates").apply { mkdirs() }
        val destinationFile = File(destinationDir, "$displayName.apk")
        if (destinationFile.exists())
            destinationFile.delete() // Always fetch a fresh copy rather than risk installing a stale/partial leftover

        val request = DownloadManager.Request(apkUrl.toUri())
            .setTitle(context.getString(R.string.update_downloading_title))
            .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
            .setDestinationUri(Uri.fromFile(destinationFile))
            .setAllowedOverMetered(true)
            .setAllowedOverRoaming(true)

        val downloadId = downloadManager.enqueue(request)

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext : Context, intent : Intent) {
                val completedId = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1L)
                if (completedId != downloadId)
                    return

                receiverContext.unregisterReceiver(this)

                val query = DownloadManager.Query().setFilterById(downloadId)
                downloadManager.query(query).use { cursor ->
                    if (!cursor.moveToFirst()) {
                        Toast.makeText(receiverContext, receiverContext.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
                        return
                    }

                    val statusIndex = cursor.getColumnIndex(DownloadManager.COLUMN_STATUS)
                    val status = if (statusIndex >= 0) cursor.getInt(statusIndex) else DownloadManager.STATUS_FAILED

                    if (status != DownloadManager.STATUS_SUCCESSFUL) {
                        Log.w(TAG, "Update download failed with status $status")
                        Toast.makeText(receiverContext, receiverContext.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
                        return
                    }
                }

                installApk(receiverContext, destinationFile)
            }
        }

        val filter = IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else
            ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
    }

    /**
     * @brief Launches the system package installer for the given APK file via a FileProvider
     *        content:// URI, as required on Android 7.0+ (raw file:// URIs are blocked there).
     */
    private fun installApk(context : Context, apkFile : File) {
        val apkUri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", apkFile)

        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(apkUri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }

        try {
            context.startActivity(intent)
        } catch (e : Exception) {
            Log.e(TAG, "Failed to launch package installer", e)
            Toast.makeText(context, context.getString(R.string.update_install_failed), Toast.LENGTH_LONG).show()
        }
    }
}
