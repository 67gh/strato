// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project

package org.stratoemu.strato

import android.app.DownloadManager
import android.app.Dialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Gravity
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.core.net.toUri
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.zip.ZipInputStream

/**
 * @brief Downloads an update, optionally extracts a compressed update archive, and launches the
 *        Android package installer. The progress dialog covers the whole operation from 0 to 100%.
 */
object UpdateInstaller {
    private const val TAG = "UpdateInstaller"
    private const val DOWNLOAD_END = 70
    private const val EXTRACTION_END = 90
    private const val POLL_INTERVAL_MS = 200L

    /**
     * @param downloadUrl URL of either the APK or the compressed update ZIP.
     * @param displayName Release tag used to create a unique local file name.
     * @param isArchive true when the release asset is the compressed update ZIP.
     */
    fun downloadAndInstall(
        context : Context,
        downloadUrl : String,
        displayName : String,
        isArchive : Boolean
    ) {
        val downloadManager = context.getSystemService(Context.DOWNLOAD_SERVICE) as? DownloadManager
        if (downloadManager == null) {
            Toast.makeText(context, context.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
            return
        }

        val progress = ProgressUi(context)
        progress.show()

        val destinationDir = File(context.getExternalFilesDir(null), "updates").apply { mkdirs() }
        val extension = if (isArchive) "zip" else "apk"
        val destinationFile = File(destinationDir, "$displayName.$extension")
        if (destinationFile.exists())
            destinationFile.delete()

        val request = DownloadManager.Request(downloadUrl.toUri())
            .setTitle(context.getString(R.string.update_downloading_title))
            .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
            .setDestinationUri(Uri.fromFile(destinationFile))
            .setAllowedOverMetered(true)
            .setAllowedOverRoaming(true)

        val downloadId = try {
            downloadManager.enqueue(request)
        } catch (e : Exception) {
            Log.e(TAG, "Failed to enqueue update download", e)
            progress.dismiss()
            Toast.makeText(context, context.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
            return
        }

        val handler = Handler(Looper.getMainLooper())
        val progressPoll = object : Runnable {
            override fun run() {
                val query = DownloadManager.Query().setFilterById(downloadId)
                downloadManager.query(query).use { cursor ->
                    if (cursor.moveToFirst()) {
                        val statusIndex = cursor.getColumnIndex(DownloadManager.COLUMN_STATUS)
                        val downloadedIndex = cursor.getColumnIndex(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR)
                        val totalIndex = cursor.getColumnIndex(DownloadManager.COLUMN_TOTAL_SIZE_BYTES)
                        val status = if (statusIndex >= 0) cursor.getInt(statusIndex) else DownloadManager.STATUS_FAILED
                        val downloaded = if (downloadedIndex >= 0) cursor.getLong(downloadedIndex) else 0L
                        val total = if (totalIndex >= 0) cursor.getLong(totalIndex) else 0L

                        if (status == DownloadManager.STATUS_RUNNING && total > 0L) {
                            val percent = (downloaded * DOWNLOAD_END / total).toInt().coerceIn(0, DOWNLOAD_END)
                            progress.update(percent, R.string.update_downloading_progress, percent)
                        }
                    }
                }
                handler.postDelayed(this, POLL_INTERVAL_MS)
            }
        }
        handler.post(progressPoll)

        val receiver = object : BroadcastReceiver() {
            override fun onReceive(receiverContext : Context, intent : Intent) {
                val completedId = intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1L)
                if (completedId != downloadId)
                    return

                try {
                    receiverContext.unregisterReceiver(this)
                } catch (_ : Exception) {
                }
                handler.removeCallbacks(progressPoll)

                val success = downloadManager.query(DownloadManager.Query().setFilterById(downloadId)).use { cursor ->
                    if (!cursor.moveToFirst()) {
                        false
                    } else {
                        val statusIndex = cursor.getColumnIndex(DownloadManager.COLUMN_STATUS)
                        val status = if (statusIndex >= 0) cursor.getInt(statusIndex) else DownloadManager.STATUS_FAILED
                        status == DownloadManager.STATUS_SUCCESSFUL
                    }
                }

                if (!success || !destinationFile.exists() || destinationFile.length() == 0L) {
                    Log.w(TAG, "Update download failed")
                    progress.dismiss()
                    Toast.makeText(receiverContext, receiverContext.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
                    return
                }

                if (!isArchive) {
                    progress.update(EXTRACTION_END, R.string.update_preparing_install)
                    installApk(receiverContext, destinationFile, progress)
                    return
                }

                progress.update(DOWNLOAD_END, R.string.update_extracting_progress, DOWNLOAD_END)
                extractApk(receiverContext, destinationFile, destinationDir, displayName, progress)
            }
        }

        val filter = IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                context.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
            else
                ContextCompat.registerReceiver(context, receiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED)
        } catch (e : Exception) {
            handler.removeCallbacks(progressPoll)
            progress.dismiss()
            Log.e(TAG, "Failed to register update receiver", e)
            Toast.makeText(context, context.getString(R.string.update_download_failed), Toast.LENGTH_LONG).show()
        }
    }

    private fun extractApk(
        context : Context,
        zipFile : File,
        destinationDir : File,
        displayName : String,
        progress : ProgressUi
    ) {
        Thread {
            try {
                val apkFile = File(destinationDir, "$displayName.apk")
                if (apkFile.exists())
                    apkFile.delete()

                var totalUncompressed = 0L
                ZipInputStream(FileInputStream(zipFile)).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        if (!entry.isDirectory && entry.name.endsWith(".apk", ignoreCase = true)) {
                            totalUncompressed += entry.size.coerceAtLeast(0L)
                        }
                        zip.closeEntry()
                    }
                }

                var extractedBytes = 0L
                ZipInputStream(FileInputStream(zipFile)).use { zip ->
                    var foundApk = false
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        if (!entry.isDirectory && entry.name.endsWith(".apk", ignoreCase = true) && !foundApk) {
                            FileOutputStream(apkFile).use { output ->
                                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                                while (true) {
                                    val count = zip.read(buffer)
                                    if (count <= 0) break
                                    output.write(buffer, 0, count)
                                    extractedBytes += count
                                    val ratio = if (totalUncompressed > 0L)
                                        (extractedBytes * 1.0 / totalUncompressed).coerceIn(0.0, 1.0)
                                    else 1.0
                                    val percent = DOWNLOAD_END + ((EXTRACTION_END - DOWNLOAD_END) * ratio).toInt()
                                    progress.postUpdate(percent, R.string.update_extracting_progress, percent)
                                }
                            }
                            foundApk = true
                        }
                        zip.closeEntry()
                    }

                    if (!foundApk)
                        throw IllegalStateException("Update archive does not contain an APK")
                }

                zipFile.delete()
                progress.postUpdate(EXTRACTION_END, R.string.update_preparing_install)
                progress.post { installApk(context, apkFile, progress) }
            } catch (e : Exception) {
                Log.e(TAG, "Failed to extract update archive", e)
                zipFile.delete()
                progress.post {
                    progress.dismiss()
                    Toast.makeText(context, context.getString(R.string.update_extract_failed), Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    /**
     * @brief Launches the Android package installer for the given APK through a FileProvider.
     * Android does not expose the package installer's internal byte-level progress to the app, so
     * the progress reaches 100% when the installer has successfully been launched.
     */
    private fun installApk(context : Context, apkFile : File, progress : ProgressUi) {
        progress.update(EXTRACTION_END, R.string.update_installing_progress)

        val apkUri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", apkFile)
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(apkUri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }

        try {
            context.startActivity(intent)
            progress.update(100, R.string.update_install_started)
            progress.dismissDelayed()
        } catch (e : Exception) {
            Log.e(TAG, "Failed to launch package installer", e)
            progress.dismiss()
            Toast.makeText(context, context.getString(R.string.update_install_failed), Toast.LENGTH_LONG).show()
        }
    }

    private class ProgressUi(private val context : Context) {
        private val dialog = Dialog(context)
        private val progressBar = ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal)
        private val message = TextView(context)
        private val handler = Handler(Looper.getMainLooper())

        init {
            val padding = (24 * context.resources.displayMetrics.density).toInt()
            val layout = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(padding, padding, padding, padding)
                gravity = Gravity.CENTER_HORIZONTAL
            }

            message.textSize = 16f
            message.setPadding(0, 0, 0, padding / 2)

            progressBar.max = 100
            progressBar.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )

            layout.addView(message, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
            layout.addView(progressBar)

            dialog.setContentView(layout)
            dialog.setTitle(context.getString(R.string.update_downloading_title))
            dialog.setCancelable(false)
            dialog.window?.setBackgroundDrawableResource(android.R.color.transparent)
        }

        fun show() {
            dialog.show()
        }

        fun update(percent : Int, messageRes : Int, vararg args : Any) {
            post {
                progressBar.progress = percent.coerceIn(0, 100)
                message.text = context.getString(messageRes, *args)
            }
        }

        fun postUpdate(percent : Int, messageRes : Int, vararg args : Any) {
            update(percent, messageRes, *args)
        }

        fun post(action : () -> Unit) {
            handler.post(action)
        }

        fun dismiss() {
            handler.post {
                if (dialog.isShowing)
                    dialog.dismiss()
            }
        }

        fun dismissDelayed() {
            handler.postDelayed({
                if (dialog.isShowing)
                    dialog.dismiss()
            }, 700L)
        }
    }
}
