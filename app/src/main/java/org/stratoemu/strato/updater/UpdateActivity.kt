// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.addCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.launch
import org.stratoemu.strato.R

class UpdateActivity : AppCompatActivity() {
    private val model: UpdateViewModel by viewModels()
    private lateinit var message: TextView
    private lateinit var bar: ProgressBar
    private lateinit var action: Button
    private var installWhenReady = false
    private val permission = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        installWhenReady = packageManager.canRequestPackageInstalls()
        if (installWhenReady && model.readyFile != null) requestInstall()
        else if (!installWhenReady) message.setText(R.string.up_permission_needed)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installWhenReady = savedInstanceState?.getBoolean("install_when_ready") ?: false
        val sourceVersion = intent.getLongExtra(EXTRA_SOURCE_VERSION, Long.MAX_VALUE)
        if (APKInstaller.installedVersion(this) > sourceVersion) {
            Toast.makeText(this, R.string.up_installed, Toast.LENGTH_LONG).show()
            finish()
            return
        }
        val url = intent.getStringExtra(EXTRA_URL)
        if (url.isNullOrBlank()) { finish(); return }
        model.start(UpdateRequest(url, intent.getStringExtra(EXTRA_LABEL).orEmpty(),
            intent.getBooleanExtra(EXTRA_ARCHIVE, false)))
        val padding = (24 * resources.displayMetrics.density).toInt()
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(padding, padding, padding, padding)
        }
        layout.addView(TextView(this).apply {
            text = getString(R.string.up_title)
            textSize = 24f
        })
        layout.addView(TextView(this).apply { text = model.request?.label })
        message = TextView(this).apply { setPadding(0, padding, 0, padding); textSize = 16f }
        bar = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100 }
        action = Button(this)
        layout.addView(message)
        layout.addView(bar)
        layout.addView(action)
        layout.addView(Button(this).apply {
            setText(R.string.up_close)
            setOnClickListener { model.cancel(); finish() }
        })
        setContentView(ScrollView(this).apply {
            isFillViewport = true
            addView(layout)
        })
        onBackPressedDispatcher.addCallback(this) { model.cancel(); finish() }
        action.setOnClickListener {
            when (model.state.value.stage) {
                UpdateStage.READY, UpdateStage.OPENED -> requestInstall()
                UpdateStage.FAILED, UpdateStage.CANCELLED -> model.retry()
                else -> Unit
            }
        }
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                model.state.collect { render(it) }
            }
        }
    }

    private fun render(state: UpdateState) {
        val transferring = state.stage == UpdateStage.DOWNLOAD || state.stage == UpdateStage.EXTRACT
        val busy = transferring || state.stage == UpdateStage.VALIDATE || state.stage == UpdateStage.IDLE
        bar.visibility = if (busy) View.VISIBLE else View.GONE
        bar.isIndeterminate = !transferring || state.total <= 0
        if (state.total > 0) bar.progress = (state.bytes.toDouble() * 100 / state.total).toInt().coerceIn(0, 100)
        message.text = when (state.stage) {
            UpdateStage.IDLE -> getString(R.string.up_preparing)
            UpdateStage.DOWNLOAD, UpdateStage.EXTRACT -> {
                val stage = getString(if (state.stage == UpdateStage.DOWNLOAD) R.string.up_downloading else R.string.up_extracting)
                if (state.total > 0) getString(R.string.up_progress, stage, bar.progress)
                else getString(R.string.up_unknown_progress, stage, state.bytes / 1024)
            }
            UpdateStage.VALIDATE -> getString(R.string.up_validating)
            UpdateStage.READY -> getString(if (state.error != 0) state.error else R.string.up_ready)
            UpdateStage.OPENED -> getString(R.string.up_installer_opened)
            UpdateStage.FAILED -> getString(state.error)
            UpdateStage.CANCELLED -> getString(R.string.up_cancelled)
        }
        action.visibility = if (busy) View.GONE else View.VISIBLE
        action.setText(if (state.stage == UpdateStage.FAILED || state.stage == UpdateStage.CANCELLED)
            R.string.up_retry else R.string.up_install)
        if (state.stage == UpdateStage.READY && installWhenReady && packageManager.canRequestPackageInstalls())
            requestInstall()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putBoolean("install_when_ready", installWhenReady)
        super.onSaveInstanceState(outState)
    }

    private fun requestInstall() {
        try {
            if (!packageManager.canRequestPackageInstalls()) {
                // Only the ActivityResult callback confirms that we may continue.
                // Do not auto-launch while the Android settings screen is still open.
                installWhenReady = false
                permission.launch(APKInstaller.permissionIntent(this))
            } else openInstaller()
        } catch (e: Exception) {
            installWhenReady = false
            model.installFailed(e)
        }
    }

    private fun openInstaller() {
        val apk = model.readyFile ?: return
        installWhenReady = false
        try {
            startActivity(APKInstaller.installIntent(this, apk))
            model.installerOpened()
        } catch (e: Exception) {
            model.installFailed(e)
        }
    }

    companion object {
        const val EXTRA_SOURCE_VERSION = "update_source_version"
        const val EXTRA_URL = "update_url"
        const val EXTRA_LABEL = "update_label"
        const val EXTRA_ARCHIVE = "update_archive"
    }
}
