/* SPDX-License-Identifier: MPL-2.0 */

package org.stratoemu.strato.diagnostics

import androidx.preference.PreferenceManager
import android.content.ClipData
import android.content.Intent
import android.os.Bundle
import android.content.DialogInterface
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
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.currentCoroutineContext
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
    private data class Session(val directory : File, val label : String, val state : String, val started : Long)
    private var sessions = emptyList<Session>()
    private var selectedName : String? = null
    private var busy = false
    private val exportSelection = mutableSetOf<String>()
    private var selectionOpen = false
    private var selectionDialog : androidx.appcompat.app.AlertDialog? = null

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        selectedName = savedInstanceState?.getString("selected_session")
        exportSelection.addAll(savedInstanceState?.getStringArrayList("export_selection").orEmpty())
        selectionOpen = savedInstanceState?.getBoolean("export_selection_open") ?: false
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
        binding.diagnosticOptions.setOnClickListener {
            val prefs = PreferenceManager.getDefaultSharedPreferences(this)
            val labels = arrayOf(getString(R.string.diagnostic_validation_off), getString(R.string.diagnostic_validation_core), getString(R.string.diagnostic_validation_sync), getString(R.string.diagnostic_validation_gpu))
            MaterialAlertDialogBuilder(this)
                .setTitle(R.string.diagnostic_options_title)
                .setSingleChoiceItems(labels, prefs.getInt("diagnostic_validation_mode", 0).coerceIn(0, 3)) { _, selected ->
                    prefs.edit().putInt("diagnostic_validation_mode", selected).apply()
                }
                .setPositiveButton(android.R.string.ok, null)
                .setNeutralButton(R.string.diagnostic_recording_options) { _, _ ->
                    MaterialAlertDialogBuilder(this)
                        .setTitle(R.string.diagnostic_recording_options)
                        .setMultiChoiceItems(
                            arrayOf(getString(R.string.diagnostic_save_shaders), getString(R.string.diagnostic_native_trace)),
                            booleanArrayOf(prefs.getBoolean("diagnostic_capture_shaders", true), prefs.getBoolean("diagnostic_native_trace", true))
                        ) { _, index, checked ->
                            val key = if (index == 0) "diagnostic_capture_shaders" else "diagnostic_native_trace"
                            prefs.edit().putBoolean(key, checked).apply()
                        }.setPositiveButton(android.R.string.ok, null).show()
                }.show()
        }
        binding.refreshSessions.setOnClickListener { refresh() }
        binding.exportDiagnostic.setOnClickListener {
            sessions.firstOrNull { it.directory.name == selectedName }?.let { export(listOf(it)) }
        }
        binding.exportDiagnostics.setOnClickListener { chooseExportRange() }
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
        outState.putStringArrayList("export_selection", ArrayList(exportSelection))
        outState.putBoolean("export_selection_open", selectionOpen)
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
        binding.exportDiagnostics.isEnabled = !busy && sessions.isNotEmpty()
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
                        Session(directory, "$game\n${DateFormat.getDateTimeInstance().format(Date(started))}", getString(state), started)
                    }
                }
                if (sessions.none { it.directory.name == selectedName })
                    selectedName = sessions.firstOrNull()?.directory?.name
                exportSelection.retainAll(sessions.map { it.directory.name }.toSet())
            } catch (e : CancellationException) {
                throw e
            } catch (_ : Exception) {
                Toast.makeText(this@DiagnosticActivity, R.string.diagnostic_read_failed, Toast.LENGTH_LONG).show()
            } finally {
                busy = false
                render()
            }
            if (selectionOpen) showExportSelection()
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

    private fun chooseExportRange() {
        if (busy || sessions.isEmpty()) return
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.diagnostic_export_multiple)
            .setItems(arrayOf(
                getString(R.string.diagnostic_selection_custom),
                getString(R.string.diagnostic_selection_today),
                getString(R.string.diagnostic_selection_week),
                getString(R.string.diagnostic_selection_all)
            )) { _, choice ->
                if (choice != 0) {
                    exportSelection.clear()
                    val window = diagnosticExportWindow(if (choice == 2) 7 else 1)
                    exportSelection.addAll(sessions.filter {
                        choice == 3 || it.started in window
                    }.map { it.directory.name })
                }
                showExportSelection()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun showExportSelection() {
        if (selectionDialog?.isShowing == true) return
        if (sessions.isEmpty()) {
            selectionOpen = false
            return
        }
        // Keep a stable snapshot while the dialog is open, even when onResume refreshes the list.
        val available = sessions.toList()
        val checked = BooleanArray(available.size) { available[it].directory.name in exportSelection }
        selectionOpen = true
        val dialog = MaterialAlertDialogBuilder(this)
            .setTitle(R.string.diagnostic_selection_custom)
            .setMultiChoiceItems(available.map { it.label }.toTypedArray(), checked) { dialog, index, value ->
                val name = available[index].directory.name
                if (value) exportSelection.add(name) else exportSelection.remove(name)
                checked[index] = value
                val alert = dialog as androidx.appcompat.app.AlertDialog
                alert.getButton(DialogInterface.BUTTON_POSITIVE).apply {
                    isEnabled = checked.any { it }
                    text = getString(R.string.diagnostic_export_count, checked.count { it })
                }
                alert.getButton(DialogInterface.BUTTON_NEUTRAL).setText(
                    if (checked.all { it }) R.string.diagnostic_selection_none else R.string.diagnostic_selection_all)
            }
            .setPositiveButton(R.string.diagnostic_export_multiple) { _, _ ->
                selectionOpen = false
                export(available.filterIndexed { index, _ -> checked[index] }, batch = true)
            }
            .setNegativeButton(android.R.string.cancel) { _, _ -> selectionOpen = false }
            .setNeutralButton(R.string.diagnostic_selection_all, null)
            .setOnCancelListener { selectionOpen = false }
            .create()
        dialog.setOnShowListener {
            fun updateButtons() {
                dialog.getButton(DialogInterface.BUTTON_POSITIVE).apply {
                    isEnabled = checked.any { it }
                    text = getString(R.string.diagnostic_export_count, checked.count { it })
                }
                dialog.getButton(DialogInterface.BUTTON_NEUTRAL).setText(
                    if (checked.all { it }) R.string.diagnostic_selection_none else R.string.diagnostic_selection_all)
            }
            dialog.getButton(DialogInterface.BUTTON_NEUTRAL).setOnClickListener {
                val select = !checked.all { it }
                available.forEachIndexed { index, session ->
                    checked[index] = select
                    dialog.listView.setItemChecked(index, select)
                    if (select) exportSelection.add(session.directory.name) else exportSelection.remove(session.directory.name)
                }
                updateButtons()
            }
            updateButtons()
        }
        selectionDialog = dialog
        dialog.setOnDismissListener { selectionDialog = null }
        dialog.show()
    }

    private fun export(selected : List<Session>, batch : Boolean = false) {
        if (busy || selected.isEmpty()) return
        busy = true
        render()
        lifecycleScope.launch {
            try {
                val file = withContext(Dispatchers.IO) {
                    val jobContext = currentCoroutineContext()
                    if (batch) DiagnosticExporter.exportBatch(applicationContext, selected.map { it.directory }) {
                        jobContext.ensureActive()
                    } else DiagnosticExporter.export(applicationContext, selected.single().directory)
                }
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
            } catch (_ : DiagnosticBatchArchive.TooLargeException) {
                Toast.makeText(this@DiagnosticActivity, R.string.diagnostic_batch_too_large, Toast.LENGTH_LONG).show()
            } catch (_ : Exception) {
                Toast.makeText(this@DiagnosticActivity, R.string.diagnostic_export_failed, Toast.LENGTH_LONG).show()
            } finally {
                busy = false
                render()
            }
        }
    }
}
