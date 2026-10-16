// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import android.app.Application
import android.content.pm.PackageInstaller
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

enum class UpdateStage { IDLE, DOWNLOAD, EXTRACT, VALIDATE, READY, INSTALLING, OPENED, INSTALLED, FAILED, CANCELLED }
data class UpdateState(val stage: UpdateStage = UpdateStage.IDLE, val bytes: Long = 0,
                       val total: Long = -1, val error: Int = 0,
                       val errorArguments: List<Any> = emptyList(), val detail: String = "")
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

    private var observeInstall = false

    init {
        viewModelScope.launch {
            UpdateSession.events.collect { event ->
                if (observeInstall && event != null && event.url == request?.url) renderInstall(event)
            }
        }
    }

    private fun renderInstall(event: InstallEvent) {
        val stage = when (event.status) {
            UpdateSession.STAGING -> UpdateStage.INSTALLING
            UpdateSession.COMMITTED, PackageInstaller.STATUS_PENDING_USER_ACTION -> UpdateStage.OPENED
            PackageInstaller.STATUS_SUCCESS -> UpdateStage.INSTALLED
            else -> if (readyFile?.isFile == true) UpdateStage.READY else UpdateStage.FAILED
        }
        val error = when (event.status) {
            PackageInstaller.STATUS_FAILURE_CONFLICT -> R.string.up_install_conflict
            PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> R.string.up_install_incompatible
            PackageInstaller.STATUS_FAILURE_STORAGE -> R.string.up_install_storage
            PackageInstaller.STATUS_FAILURE_ABORTED -> R.string.up_install_aborted
            PackageInstaller.STATUS_FAILURE_BLOCKED -> R.string.up_install_blocked
            PackageInstaller.STATUS_FAILURE_INVALID -> R.string.up_install_invalid
            UpdateSession.STAGING, UpdateSession.COMMITTED,
            PackageInstaller.STATUS_PENDING_USER_ACTION, PackageInstaller.STATUS_SUCCESS -> 0
            else -> R.string.update_install_failed
        }
        mutableState.value = UpdateState(stage, error = error,
            detail = if (event.status >= 0) "Android status=${event.status}\n${event.detail}" else "")
    }

    fun start(value: UpdateRequest) {
        if (request != null) return
        request = value
        val previous = UpdateSession.restore(getApplication())
        if (previous != null && previous.url == value.url && previous.status != PackageInstaller.STATUS_SUCCESS) {
            observeInstall = true
            renderInstall(previous)
        } else retry()
    }

    fun retry() {
        if (job?.isActive == true) return
        val value = request ?: return
        observeInstall = false
        UpdateSession.abandon(getApplication())
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
                        val archive = File(directory, "download.payload")
                        try {
                            // A release can replace an asset at the same URL. Never reuse
                            // the old completed APK on an explicit new attempt.
                            result.delete()
                            transfer.download(value.url, archive,
                                { cancellation.ensureActive() }) { bytes, total ->
                                mutableState.value = UpdateState(UpdateStage.DOWNLOAD, bytes, total)
                            }
                            cancellation.ensureActive()
                            failureMessage = R.string.up_payload_error
                            mutableState.value = UpdateState(UpdateStage.VALIDATE)
                            APKPayload.prepare(archive, result, { cancellation.ensureActive() }, {
                                mutableState.value = UpdateState(UpdateStage.EXTRACT)
                            }) { bytes, total ->
                                mutableState.value = UpdateState(UpdateStage.EXTRACT, bytes, total)
                            }
                            cancellation.ensureActive()
                            failureMessage = R.string.up_validation_error
                            val version = APKInstaller.validate(getApplication(), result)
                            if (version == null)
                                Log.w("StratoUpdater", "Local APK parser returned null; deferring to PackageInstaller")
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
            } catch (e: APKValidationException) {
                Log.w("StratoUpdater", "Update APK rejected", e)
                val (message, arguments) = when (e.reason) {
                    APKValidationFailure.UNREADABLE -> R.string.up_invalid_apk to emptyList()
                    APKValidationFailure.DIFFERENT_PACKAGE -> R.string.up_wrong_variant to
                        listOf(e.candidate!!.packageName, e.installed!!.packageName)
                    APKValidationFailure.OLDER_VERSION -> R.string.up_older_apk to
                        listOf(e.candidate!!.versionCode, e.installed!!.versionCode)
                }
                mutableState.value = UpdateState(UpdateStage.FAILED, error = message,
                    errorArguments = arguments)
            } catch (e: Exception) {
                Log.w("StratoUpdater", "Update preparation failed", e)
                mutableState.value = UpdateState(UpdateStage.FAILED, error = failureMessage,
                    detail = e.toString())
            } finally {
                downloader = null
            }
        }
    }

    fun cancel() {
        job?.cancel()
        downloader?.cancel()
    }

    fun install() {
        if (job?.isActive == true) return
        val apk = readyFile ?: run { retry(); return }
        val value = request ?: return
        observeInstall = true
        mutableState.value = UpdateState(UpdateStage.INSTALLING)
        job = viewModelScope.launch {
            try {
                withContext(Dispatchers.IO) {
                    transferLock.withLock {
                        val cancellation = currentCoroutineContext()
                        UpdateSession.install(getApplication(), apk, value.url) { cancellation.ensureActive() }
                    }
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                installFailed(e)
            }
        }
    }

    fun installFailed(error: Exception) {
        Log.w("StratoUpdater", "Cannot open update installer", error)
        // Keep the completed APK available for another installation attempt.
        mutableState.value = UpdateState(UpdateStage.READY, error = R.string.update_install_failed, detail = error.toString())
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
