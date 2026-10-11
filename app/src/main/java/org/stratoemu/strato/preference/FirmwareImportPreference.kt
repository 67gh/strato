/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2023 Strato Team and Contributors (https://github.com/strato-emu/)
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.net.Uri
import android.util.AttributeSet
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.preference.Preference
import androidx.preference.Preference.SummaryProvider
import com.google.android.material.snackbar.Snackbar
import org.stratoemu.strato.R
import org.stratoemu.strato.fragments.IndeterminateProgressDialogFragment
import org.stratoemu.strato.getPublicFilesDir
import org.stratoemu.strato.settings.SettingsActivity
import org.stratoemu.strato.utils.ZipUtils
import org.stratoemu.strato.utils.DirectoryReplacement
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class FirmwareImportPreference @JvmOverloads constructor(context : Context, attrs : AttributeSet? = null, defStyleAttr : Int = androidx.preference.R.attr.preferenceStyle) : Preference(context, attrs, defStyleAttr) {
    private class Firmware(val valid : Boolean, val version : String)

    private val firmwarePath = File(context.getPublicFilesDir().canonicalPath + "/switch/nand/system/Contents/registered/")
    private val keysPath = "${context.filesDir.canonicalPath}/keys/"
    private val fontsPath = "${context.getPublicFilesDir().canonicalPath}/fonts/"

    private val documentPicker = (context as ComponentActivity).registerForActivityResult(ActivityResultContracts.OpenDocument()) {
        it?.let { uri ->
            val task : () -> Unit = {
                val result = importFirmware(uri)
                CoroutineScope(Dispatchers.Main).launch {
                    result.version?.let { version ->
                        persistString(version)
                        notifyChanged()
                    }
                    Snackbar.make((context as SettingsActivity).binding.root, result.message, Snackbar.LENGTH_LONG).show()
                }
            }

            IndeterminateProgressDialogFragment.newInstance(context as SettingsActivity, R.string.import_firmware_in_progress, task)
                .show(context.supportFragmentManager, IndeterminateProgressDialogFragment.TAG)
        }
    }

    /**
     * @param message The string resource describing the outcome
     * @param version The installed firmware version, null if nothing was installed
     */
    class ImportResult(val message : Int, val version : String?)

    /**
     * Installs the firmware from a zip, this blocks so it must run on a background thread
     * The previous firmware is only replaced once the new one was found to be valid
     */
    fun importFirmware(uri : Uri) : ImportResult {
        var workDir : File? = null
        return try {
            val work = Files.createTempDirectory(context.cacheDir.toPath(), "firmware-").toFile()
            workDir = work
            val cacheFirmwareDir = File(work, "registered")
            val extractedDir = File(work, "extracted")
            val stagedFonts = File(work, "fonts")
            val inputZip = context.contentResolver.openInputStream(uri) ?: throw IOException("Cannot open firmware archive")
            // Unzip in cache dir to not delete previous firmware in case the zip given doesn't contain a valid one
            inputZip.use { ZipUtils.unzip(it, extractedDir) }

            // A full firmware has its archives at the root of the zip but a zip made by hand (a lite firmware) often has them in a folder, only the NCAs matter
            cacheFirmwareDir.mkdirs()
            extractedDir.walkTopDown().filter { it.isFile && it.name.endsWith(".nca", ignoreCase = true) }.forEach {
                it.copyTo(File(cacheFirmwareDir, it.name), overwrite = false)
            }

            val firmware = isFirmwareValid(cacheFirmwareDir)
            if (!firmware.valid) {
                ImportResult(R.string.import_firmware_invalid_contents, null)
            } else {
                stagedFonts.mkdirs()
                extractFonts(cacheFirmwareDir.path, keysPath, stagedFonts.path + "/")
                if (stagedFonts.listFiles()?.isNotEmpty() == true) {
                    DirectoryReplacement.replace(File(fontsPath)) { staging ->
                        val previousFonts = File(fontsPath)
                        if (previousFonts.exists() && !previousFonts.copyRecursively(staging, true))
                            throw IOException("Cannot retain existing fonts")
                        if (!stagedFonts.copyRecursively(staging, true)) throw IOException("Cannot copy fonts")
                    }
                }
                DirectoryReplacement.replace(firmwarePath) { staging ->
                    if (!cacheFirmwareDir.copyRecursively(staging, true)) throw IOException("Cannot copy firmware")
                }
                writeLiteFirmware()
                ImportResult(R.string.import_firmware_success, firmware.version)
            }
        } catch (e : Exception) {
            ImportResult(R.string.error, null)
        } finally {
            workDir?.deleteRecursively()
        }
    }

    /**
     * Writes firmware_lite.zip in the public files directory: only the archives the emulator needs (the version and the shared fonts) out of the installed firmware
     * It's a valid firmware package on its own, so it can be imported on another device instead of the full firmware
     * Failing to write it never fails the import
     */
    private fun writeLiteFirmware() {
        try {
            val essential = listEssentialArchives(firmwarePath.path, keysPath)
            if (essential.isEmpty())
                return

            val liteZip = File(context.getPublicFilesDir(), "firmware_lite.zip")
            ZipOutputStream(liteZip.outputStream().buffered()).use { zip ->
                for (name in essential) {
                    zip.putNextEntry(ZipEntry(name))
                    File(firmwarePath, name).inputStream().use { it.copyTo(zip) }
                    zip.closeEntry()
                }
            }
        } catch (e : Exception) {
            // The lite zip is only a convenience
        }
    }

    init {
        val keysDir = File(keysPath)
        isEnabled = keysDir.exists() && keysDir.listFiles()?.isNotEmpty() == true

        summaryProvider = SummaryProvider<FirmwareImportPreference> { preference ->
            val defaultString = if (preference.isEnabled)
                context.getString(R.string.firmware_not_installed)
            else
                context.getString(R.string.firmware_keys_needed)

            getPersistedString(defaultString)
        }
    }

    override fun onClick() = documentPicker.launch(arrayOf("application/zip"))

    /**
     * Checks if the given directory (that only contains the NCAs found in the zip) stores a usable firmware: one of the NCAs stores the firmware version or is a shared font
     * A lite firmware (the version and the fonts) is as valid as a full one
     * @return A pair that tells if the firmware is valid, and if so, which firmware version it is
     */
    private fun isFirmwareValid(cacheFirmwareDir : File) : Firmware {
        if (cacheFirmwareDir.list()?.isEmpty() != false)
            return Firmware(false, "")

        val version = fetchFirmwareVersion(cacheFirmwareDir.path, keysPath)
        if (version.isNotEmpty())
            return Firmware(true, version)

        // A hand-made lite firmware may only have the fonts, it's still worth installing
        return if (listEssentialArchives(cacheFirmwareDir.path, keysPath).isNotEmpty()) Firmware(true, "Lite") else Firmware(false, "")
    }

    private external fun fetchFirmwareVersion(systemArchivesPath : String, keysPath : String) : String
    private external fun extractFonts(systemArchivesPath : String, keysPath : String, fontsPath : String)
    private external fun listEssentialArchives(systemArchivesPath : String, keysPath : String) : Array<String>
}
