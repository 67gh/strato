/*
 * SPDX-License-Identifier: MPL-2.0
 */

package org.stratoemu.strato

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.DocumentsContract
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.lifecycle.lifecycleScope
import androidx.preference.PreferenceManager
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.stratoemu.strato.databinding.SetupActivityBinding
import org.stratoemu.strato.databinding.SetupStepBinding
import org.stratoemu.strato.di.getSettings
import org.stratoemu.strato.preference.FirmwareImportPreference
import org.stratoemu.strato.utils.WindowInsetsHelper
import java.io.File

/**
 * Shown on the first launch: asks for the permissions and takes the game folders, the keys and the firmware, so that everything the emulator needs is set up
 * before the game list is shown. Every step can be skipped and done later from the settings
 */
class SetupActivity : AppCompatActivity() {
    private val binding by lazy { SetupActivityBinding.inflate(layoutInflater) }
    private val settings by lazy { getSettings() }

    private lateinit var notifications : SetupStepBinding
    private lateinit var storage : SetupStepBinding
    private lateinit var prodKeys : SetupStepBinding
    private lateinit var titleKeys : SetupStepBinding
    private lateinit var firmware : SetupStepBinding

    /**
     * Only used for its [FirmwareImportPreference.importFirmware], it has to be created in [onCreate] as it registers an activity result launcher
     */
    private lateinit var firmwareImporter : FirmwareImportPreference

    private val keysDirectory get() = File(filesDir, "keys")

    private val notificationPermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) { refresh() }

    private val folderPicker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) {
        it?.let { uri ->
            contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            settings.searchLocationList = settings.searchLocationList + uri
            settings.refreshRequired = true
            refresh()
        }
    }

    private val prodKeysPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { importKeys(it, KeyReader.KeyType.Prod) }
    private val titleKeysPicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { importKeys(it, KeyReader.KeyType.Title) }

    private val firmwarePicker = registerForActivityResult(ActivityResultContracts.OpenDocument()) {
        it?.let { uri ->
            firmware.stepButton.isEnabled = false
            lifecycleScope.launch {
                val result = withContext(Dispatchers.IO) { firmwareImporter.importFirmware(uri) }
                result.version?.let { version -> PreferenceManager.getDefaultSharedPreferences(this@SetupActivity).edit().putString("firmware", version).apply() }
                Snackbar.make(binding.root, result.message, Snackbar.LENGTH_LONG).show()
                refresh()
            }
        }
    }

    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(binding.root)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsHelper.applyToActivity(binding.root)

        firmwareImporter = FirmwareImportPreference(this)

        notifications = addStep(R.string.setup_notifications, R.string.setup_notifications_desc)
        storage = addStep(R.string.setup_storage, R.string.setup_storage_desc)
        prodKeys = addStep(R.string.prod_keys, R.string.setup_prod_keys_desc)
        titleKeys = addStep(R.string.title_keys, R.string.setup_title_keys_desc)
        firmware = addStep(R.string.firmware, R.string.setup_firmware_desc)

        notifications.stepButton.setText(R.string.setup_allow)
        notifications.stepButton.setOnClickListener {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        storage.stepButton.setText(R.string.add_folder)
        storage.stepButton.setOnClickListener { folderPicker.launch(null) }

        prodKeys.stepButton.setText(R.string.setup_choose)
        prodKeys.stepButton.setOnClickListener { prodKeysPicker.launch(arrayOf("*/*")) }

        titleKeys.stepButton.setText(R.string.setup_choose)
        titleKeys.stepButton.setOnClickListener { titleKeysPicker.launch(arrayOf("*/*")) }

        firmware.stepButton.setText(R.string.setup_choose)
        firmware.stepButton.setOnClickListener { firmwarePicker.launch(arrayOf("application/zip")) }

        binding.setupFinish.setOnClickListener {
            setResult(RESULT_OK)
            finish()
        }

        refresh()
    }

    /**
     * Leaving the screen in any way (the finish button or back) counts as having done the setup, it's not shown again
     */
    override fun finish() {
        settings.setupCompleted = true
        super.finish()
    }

    private fun addStep(title : Int, description : Int) : SetupStepBinding {
        val step = SetupStepBinding.inflate(layoutInflater, binding.setupSteps, false)
        step.stepTitle.setText(title)
        step.stepDescription.setText(description)
        binding.setupSteps.addView(step.root)
        return step
    }

    private fun importKeys(uri : Uri?, type : KeyReader.KeyType) {
        if (uri == null)
            return

        contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        settings.refreshRequired = true

        val message = when (KeyReader.import(this, uri, type)) {
            KeyReader.ImportResult.Success -> R.string.import_keys_success
            KeyReader.ImportResult.InvalidInputPath -> R.string.import_keys_invalid_input_path
            KeyReader.ImportResult.InvalidKeys -> R.string.import_keys_invalid_keys
            KeyReader.ImportResult.DeletePreviousFailed -> R.string.import_keys_delete_previous_failed
            KeyReader.ImportResult.MoveFailed -> R.string.import_keys_move_failed
        }
        Snackbar.make(binding.root, message, Snackbar.LENGTH_LONG).show()
        refresh()
    }

    private fun SetupStepBinding.showStatus(text : String?) {
        stepStatus.text = text
        stepStatus.visibility = if (text != null) android.view.View.VISIBLE else android.view.View.GONE
    }

    /**
     * Updates what every step shows from the current state
     */
    private fun refresh() {
        val notificationsAllowed = Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED
        notifications.showStatus(if (notificationsAllowed) getString(R.string.setup_notifications_granted) else null)
        notifications.stepButton.isEnabled = !notificationsAllowed

        val folders = settings.searchLocationList
        storage.showStatus(if (folders.isEmpty()) null else folders.joinToString("\n") { folderName(it) })

        val prodKeysImported = File(keysDirectory, KeyReader.KeyType.Prod.fileName).exists()
        prodKeys.showStatus(if (prodKeysImported) getString(R.string.setup_imported) else null)
        titleKeys.showStatus(if (File(keysDirectory, KeyReader.KeyType.Title.fileName).exists()) getString(R.string.setup_imported) else null)

        // The firmware can only be read with the production keys
        val firmwareVersion = PreferenceManager.getDefaultSharedPreferences(this).getString("firmware", null)
        firmware.showStatus(firmwareVersion)
        firmware.stepButton.isEnabled = prodKeysImported

        binding.setupFinish.setText(if (folders.isNotEmpty() || prodKeysImported) R.string.setup_finish else R.string.setup_skip)
    }

    private fun folderName(uri : Uri) : String = try {
        Uri.decode(DocumentsContract.getTreeDocumentId(uri))
    } catch (e : Exception) {
        Uri.decode(uri.toString())
    }
}
