/*
 * SPDX-License-Identifier: MPL-2.0
 */

package org.stratoemu.strato.preference

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import android.util.AttributeSet
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.preference.Preference
import androidx.preference.Preference.SummaryProvider
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.stratoemu.strato.R
import org.stratoemu.strato.di.getSettings

/**
 * Manages the list of game folders: the dialog lists them, unchecking one removes it and a button adds another one
 */
class SearchLocationsPreference @JvmOverloads constructor(context : Context, attrs : AttributeSet? = null, defStyleAttr : Int = androidx.preference.R.attr.preferenceStyle) : Preference(context, attrs, defStyleAttr) {
    private val documentPicker = (context as ComponentActivity).registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) {
        it?.let { uri ->
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)

            val settings = context.getSettings()
            settings.searchLocationList = settings.searchLocationList + uri
            settings.refreshRequired = true
            notifyChanged()
            showDialog() // Back to the list so more folders can be added
        }
    }

    init {
        summaryProvider = SummaryProvider<SearchLocationsPreference> {
            val locations = context.getSettings().searchLocationList
            when (locations.size) {
                0 -> context.getString(R.string.no_search_locations)
                1 -> displayName(locations[0])
                else -> context.resources.getQuantityString(R.plurals.search_locations_count, locations.size, locations.size)
            }
        }
    }

    override fun onClick() = showDialog()

    private fun displayName(uri : Uri) : String = try {
        Uri.decode(DocumentsContract.getTreeDocumentId(uri))
    } catch (e : Exception) {
        Uri.decode(uri.toString())
    }

    private fun showDialog() {
        val settings = context.getSettings()
        val locations = settings.searchLocationList

        val builder = MaterialAlertDialogBuilder(context)
            .setTitle(R.string.search_locations)
            .setNeutralButton(R.string.add_folder) { _, _ -> documentPicker.launch(null) }
            .setNegativeButton(android.R.string.cancel, null)

        if (locations.isEmpty()) {
            builder.setMessage(R.string.no_search_locations)
        } else {
            val checked = BooleanArray(locations.size) { true }
            builder
                .setMultiChoiceItems(locations.map { displayName(it) }.toTypedArray(), checked) { _, index, isChecked -> checked[index] = isChecked }
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    val kept = locations.filterIndexed { index, _ -> checked[index] }
                    if (kept.size != locations.size) {
                        // The folders that were unchecked are removed, the permission is released too
                        locations.filterIndexed { index, _ -> !checked[index] }.forEach {
                            try {
                                context.contentResolver.releasePersistableUriPermission(it, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                            } catch (_ : SecurityException) {
                            }
                        }
                        settings.searchLocationList = kept
                        settings.refreshRequired = true
                        notifyChanged()
                    }
                }
        }
        builder.show()
    }
}
