/*
 * SPDX-License-Identifier: MPL-2.0
 */

package org.stratoemu.strato.settings

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.annotation.DrawableRes
import androidx.annotation.StringRes
import androidx.core.content.edit
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import androidx.preference.PreferenceManager
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.stratoemu.strato.R
import org.stratoemu.strato.UpdateChecker
import org.stratoemu.strato.UpdateInstaller
import org.stratoemu.strato.databinding.FragmentSettingsHomeBinding
import org.stratoemu.strato.databinding.SettingsHomeItemBinding
import org.stratoemu.strato.utils.WindowInsetsHelper
import org.xmlpull.v1.XmlPullParser
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The landing page of the settings, a list of categories that each open their own page
 * The page of a category is a [SettingsActivity] showing a [GlobalSettingsFragment] filtered down to the preference categories listed in the entry
 */
class SettingsHomeFragment : Fragment() {
    /**
     * @param categories The keys of the preference categories shown in the page, null if the entry is an action rather than a page
     */
    private class Entry(
        @StringRes val title : Int,
        @DrawableRes val icon : Int,
        val categories : Array<String>? = null,
        val action : Action? = null
    )

    private enum class Action {
        ABOUT
    }

    private val entries = listOf(
        Entry(R.string.general, R.drawable.ic_settings_general, arrayOf("category_content", "category_appearance")),
        Entry(R.string.system, R.drawable.ic_settings_system, arrayOf("category_system")),
        Entry(R.string.settings_controllers, R.drawable.ic_settings_controller, arrayOf("category_input")),
        Entry(R.string.settings_graphics, R.drawable.ic_settings_graphics, arrayOf("category_gpu", "category_hacks")),
        Entry(R.string.settings_layout, R.drawable.ic_settings_layout, arrayOf("category_presentation")),
        Entry(R.string.audio, R.drawable.ic_settings_audio, arrayOf("category_audio")),
        Entry(R.string.debug, R.drawable.ic_settings_debug, arrayOf("category_debug")),
        Entry(R.string.licenses, R.drawable.ic_settings_licenses, arrayOf("category_licenses")),
        Entry(R.string.about, R.drawable.ic_settings_licenses, action = Action.ABOUT),
        Entry(R.string.settings_defaults, R.drawable.ic_settings_defaults)
    )

    override fun onCreateView(inflater : LayoutInflater, container : ViewGroup?, savedInstanceState : Bundle?) : View {
        val binding = FragmentSettingsHomeBinding.inflate(inflater, container, false)
        binding.settingsHomeList.layoutManager = LinearLayoutManager(requireContext())
        binding.settingsHomeList.adapter = EntryAdapter()
        WindowInsetsHelper.setPadding(binding.settingsHomeList, bottom = true)
        return binding.root
    }

    private fun onEntryClicked(entry : Entry) {
        entry.action?.let {
            when (it) {
                Action.ABOUT -> showAboutDialog()
            }
            return
        }

        val categories = entry.categories
        if (categories == null) {
            confirmReset(entry)
            return
        }

        startActivity(Intent(requireContext(), SettingsActivity::class.java).apply {
            putExtra(SettingsActivity.EXTRA_TITLE, entry.title)
            putExtra(SettingsActivity.EXTRA_CATEGORIES, categories)
        })
    }

    private fun showAboutDialog() {
        val context = requireContext()
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.about)
            .setMessage(getString(R.string.about_message, org.stratoemu.strato.BuildConfig.VERSION_NAME))
            .setPositiveButton(R.string.check_for_updates) { _, _ ->
                checkForUpdatesManually()
            }
            .setNegativeButton(android.R.string.ok, null)
            .show()
    }

    private fun checkForUpdatesManually() {
        lifecycleScope.launch {
            val currentBuildTimeMs = org.stratoemu.strato.BuildConfig.BUILD_TIMESTAMP * 1000L

            val updateInfo = withContext(Dispatchers.IO) {
                UpdateChecker.checkForUpdate(
                    "67gh",
                    "strato",
                    currentBuildTimeMs,
                    org.stratoemu.strato.BuildConfig.BUILD_COMMIT_FULL
                )
            }

            if (!isAdded) return@launch

            if (updateInfo == null) {
                showUpdateCheckResult(getString(R.string.update_up_to_date))
                return@launch
            }

            val apkUrl = updateInfo.apkDownloadUrl
            MaterialAlertDialogBuilder(requireContext())
                .setTitle(R.string.update_available_title)
                .setMessage(getString(R.string.update_available_message, updateInfo.tagName))
                .setPositiveButton(R.string.update_available_action) { _, _ ->
                    if (apkUrl != null) {
                        UpdateInstaller.downloadAndInstall(requireContext(), apkUrl, updateInfo.tagName)
                    } else {
                        startActivity(Intent(Intent.ACTION_VIEW, android.net.Uri.parse(updateInfo.releaseUrl)))
                    }
                }
                .setNegativeButton(R.string.update_available_dismiss, null)
                .show()
        }
    }

    private fun showUpdateCheckResult(message : String) {
        if (!isAdded) return
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.check_for_updates)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun confirmReset(entry : Entry) {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(entry.title)
            .setMessage(R.string.settings_defaults_warning)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                // Only the emulation settings are reset, the content paths, keys and controller bindings are kept
                val sharedPreferences = PreferenceManager.getDefaultSharedPreferences(requireContext())
                sharedPreferences.edit { emulationPreferenceKeys().forEach { remove(it) } }
                requireActivity().recreate()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /**
     * @return The keys of every preference declared in the emulation preferences XML
     */
    private fun emulationPreferenceKeys() : Set<String> {
        val keys = mutableSetOf<String>()
        val parser = resources.getXml(R.xml.emulation_preferences)
        try {
            while (parser.eventType != XmlPullParser.END_DOCUMENT) {
                if (parser.eventType == XmlPullParser.START_TAG) {
                    for (index in 0 until parser.attributeCount) {
                        if (parser.getAttributeName(index) == "key")
                            keys.add(parser.getAttributeValue(index))
                    }
                }
                parser.next()
            }
        } finally {
            parser.close()
        }
        return keys
    }

    private inner class EntryAdapter : RecyclerView.Adapter<EntryAdapter.ViewHolder>() {
        inner class ViewHolder(val binding : SettingsHomeItemBinding) : RecyclerView.ViewHolder(binding.root)

        override fun onCreateViewHolder(parent : ViewGroup, viewType : Int) = ViewHolder(SettingsHomeItemBinding.inflate(LayoutInflater.from(parent.context), parent, false))

        override fun getItemCount() = entries.size

        override fun onBindViewHolder(holder : ViewHolder, position : Int) {
            val entry = entries[position]
            holder.binding.icon.setImageResource(entry.icon)
            holder.binding.title.setText(entry.title)
            holder.binding.root.setOnClickListener { onEntryClicked(entry) }
        }
    }
}
