/* SPDX-License-Identifier: MPL-2.0 */

package org.stratoemu.strato.diagnostics

import android.content.ClipData
import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.FileProvider
import androidx.core.view.WindowCompat
import androidx.core.view.isVisible
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.stratoemu.strato.R
import org.stratoemu.strato.databinding.DiagnosticActivityBinding
import org.stratoemu.strato.utils.WindowInsetsHelper
import java.io.File
import java.text.DateFormat
import java.util.Date

/** Reads persisted sessions, including runs whose emulation process no longer exists. */
class DiagnosticActivity : AppCompatActivity() {
    private val binding by lazy { DiagnosticActivityBinding.inflate(layoutInflater) }
    private data class Session(val directory : File, val label : String, val state : String)
    private var sessions = emptyList<Session>()
    private var selectedName : String? = null
    private var busy = false

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        selectedName = savedInstanceState?.getString("selected_session")
        setContentView(binding.root)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsHelper.applyToActivity(binding.root, binding.content)
        setSupportActionBar(binding.titlebar.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        supportActionBar?.setTitle(R.string.diagnostic_title)
        binding.chooseSession.setOnClickListener {
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.diagnostic_choose_session)
                .setItems(sessions.map { it.label }.toTypedArray()) { _, index ->
                    selectedName = sessions[index].directory.name
                    render()
                }
                .show()
        }
        binding.refreshSessions.setOnClickListener { refresh() }
        binding.exportDiagnostic.setOnClickListener { export() }
        binding.deleteSession.setOnClickListener {
            val session = sessions.firstOrNull { it.directory.name == selectedName } ?: return@setOnClickListener
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.diagnostic_delete)
                .setMessage(getString(R.string.diagnostic_delete_confirmation, session.label))
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.diagnostic_delete) { _, _ -> delete(session) }
                .show()
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    override fun onSaveInstanceState(outState : Bundle) {
        outState.putString("selected_session", selectedName)
        super.onSaveInstanceState(outState)
    }

    override fun onSupportNavigateUp() : Boolean {
        finish()
        return true
    }

    private fun render() {
        val selected = sessions.firstOrNull { it.directory.name == selectedName }
        binding.sessionLabel.text = selected?.label ?: getString(R.string.diagnostic_no_sessions)
        binding.sessionStatus.text = selected?.state ?: ""
        binding.progress.isVisible = busy
        binding.chooseSession.isEnabled = !busy && sessions.isNotEmpty()
        binding.exportDiagnostic.isEnabled = !busy && selected != null
        binding.deleteSession.isEnabled = !busy && selected != null
        binding.refreshSessions.isEnabled = !busy
    }

    private fun refresh() {
        if (busy) return
        busy = true
        render()
        lifecycleScope.launch {
            try {
                sessions = withContext(Dispatchers.IO) {
                    DiagnosticSession.listSessions(applicationContext).map { directory ->
                        val metadata = runCatching { JSONObject(File(directory, "session.json").readText()) }.getOrNull()
                        val started = metadata?.optLong("started_at_epoch_ms", directory.lastModified()) ?: directory.lastModified()
                        val game = metadata?.optString("game_name")?.takeIf { it.isNotBlank() } ?: getString(R.string.diagnostic_unknown_game)
                        val state = when {
                            DiagnosticSession.isSessionActive(directory) -> R.string.diagnostic_running
                            metadata?.optString("status") == "finished" -> R.string.diagnostic_ended
                            else -> R.string.diagnostic_interrupted
                        }
                        Session(directory, "$game\n${DateFormat.getDateTimeInstance().format(Date(started))}", getString(state))
                    }
                }
                if (sessions.none { it.directory.name == selectedName })
                    selectedName = sessions.firstOrNull()?.directory?.name
            } catch (e : CancellationException) {
                throw e
            } catch (_ : Exception) {
                Toast.makeText(this@DiagnosticActivity, R.string.diagnostic_read_failed, Toast.LENGTH_LONG).show()
            } finally {
                busy = false
                render()
            }
        }
    }

    private fun delete(session : Session) {
        if (busy) return
        busy = true
        render()
        lifecycleScope.launch {
            try {
                withContext(Dispatchers.IO) { DiagnosticExporter.deleteSession(applicationContext, session.directory) }
            } catch (e : CancellationException) {
                throw e
            } catch (_ : Exception) {
                Toast.makeText(this@DiagnosticActivity, R.string.diagnostic_delete_failed, Toast.LENGTH_LONG).show()
            } finally {
                busy = false
                render()
            }
            refresh()
        }
    }

    private fun export() {
        val session = sessions.firstOrNull { it.directory.name == selectedName } ?: return
        if (busy) return
        busy = true
        render()
        lifecycleScope.launch {
            try {
                val file = withContext(Dispatchers.IO) { DiagnosticExporter.export(applicationContext, session.directory) }
                val uri = FileProvider.getUriForFile(this@DiagnosticActivity, "$packageName.fileprovider", file)
                val share = Intent(Intent.ACTION_SEND).apply {
                    type = "application/zip"
                    putExtra(Intent.EXTRA_STREAM, uri)
                    clipData = ClipData.newRawUri(getString(R.string.diagnostic_title), uri)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                startActivity(Intent.createChooser(share, getString(R.string.diagnostic_export)))
            } catch (e : CancellationException) {
                throw e
            } catch (_ : Exception) {
                Toast.makeText(this@DiagnosticActivity, R.string.diagnostic_export_failed, Toast.LENGTH_LONG).show()
            } finally {
                busy = false
                render()
            }
        }
    }
}
