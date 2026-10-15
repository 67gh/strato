// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import android.app.Application
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.stratoemu.strato.R
import java.io.File
import java.security.MessageDigest

enum class UpdateStage { IDLE, DOWNLOAD, EXTRACT, VALIDATE, READY, OPENED, FAILED, CANCELLED }
data class UpdateState(val stage: UpdateStage = UpdateStage.IDLE, val bytes: Long = 0,
                       val total: Long = -1, val error: Int = 0)
data class UpdateRequest(val url: String, val label: String, val archive: Boolean)

/** Holds no Activity or View; rotation keeps the transfer, closing the screen cancels it. */
class UpdateViewModel(application: Application) : AndroidViewModel(application) {
    private val mutableState = MutableStateFlow(UpdateState())
    val state = mutableState.asStateFlow()
    private var job: Job? = null
    private var downloader: APKDownloader? = null
    var request: UpdateRequest? = null
        private set
    var readyFile: File? = null
        private set

    fun start(value: UpdateRequest) {
        if (request != null) return
        request = value
        retry()
    }

    fun retry() {
        if (job?.isActive == true) return
        val value = request ?: return
        readyFile = null
        val transfer = APKDownloader()
        downloader = transfer
        mutableState.value = UpdateState(UpdateStage.DOWNLOAD)
        job = viewModelScope.launch {
            var failureMessage = R.string.up_download_error
            try {
                val apk = withContext(Dispatchers.IO) {
                    transferLock.withLock {
                        val cancellation = currentCoroutineContext()
                        val id = MessageDigest.getInstance("SHA-256").digest(value.url.toByteArray())
                            .joinToString("") { "%02x".format(it) }
                        val directory = File(getApplication<Application>().cacheDir, "updates/$id")
                        val result = File(directory, "update.apk")
                        val archive = File(directory, "download.zip")
                        try {
                            if (!result.isFile) {
                                transfer.download(value.url, if (value.archive) archive else result,
                                    { cancellation.ensureActive() }) { bytes, total ->
                                    mutableState.value = UpdateState(UpdateStage.DOWNLOAD, bytes, total)
                                }
                                if (value.archive) {
                                    failureMessage = R.string.up_extract_error
                                    mutableState.value = UpdateState(UpdateStage.EXTRACT)
                                    UpdateFiles.extractApk(archive, result, { cancellation.ensureActive() }) { bytes, total ->
                                        mutableState.value = UpdateState(UpdateStage.EXTRACT, bytes, total)
                                    }
                                }
                            }
                            cancellation.ensureActive()
                            failureMessage = R.string.up_invalid_apk
                            mutableState.value = UpdateState(UpdateStage.VALIDATE)
                            APKInstaller.validate(getApplication(), result)
                            result
                        } catch (e: Exception) {
                            result.delete()
                            throw e
                        } finally {
                            archive.delete()
                        }
                    }
                }
                readyFile = apk
                mutableState.value = UpdateState(UpdateStage.READY)
            } catch (e: CancellationException) {
                mutableState.value = UpdateState(UpdateStage.CANCELLED)
                throw e
            } catch (e: Exception) {
                Log.w("StratoUpdater", "Update preparation failed", e)
                mutableState.value = UpdateState(UpdateStage.FAILED, error = failureMessage)
            } finally {
                downloader = null
            }
        }
    }

    fun cancel() {
        job?.cancel()
        downloader?.cancel()
    }

    fun installerOpened() { mutableState.value = UpdateState(UpdateStage.OPENED) }

    fun installFailed(error: Exception) {
        Log.w("StratoUpdater", "Cannot open update installer", error)
        // Keep the completed APK available for another installation attempt.
        mutableState.value = UpdateState(UpdateStage.READY, error = R.string.update_install_failed)
    }

    companion object {
        // A just-closed screen may still be cleaning its partial file while a new
        // screen starts. Serialize access so the old cleanup cannot erase a retry.
        private val transferLock = Mutex()
    }

    override fun onCleared() {
        cancel()
        super.onCleared()
    }
}
