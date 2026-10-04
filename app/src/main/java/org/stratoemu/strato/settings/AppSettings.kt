/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato.settings

import android.content.Context
import android.net.Uri
import dagger.hilt.android.qualifiers.ApplicationContext
import org.stratoemu.strato.utils.sharedPreferences
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Settings used by the app globally
 */
@Singleton
class AppSettings @Inject constructor(@ApplicationContext private val context : Context) {
    // Content
    var searchLocation by sharedPreferences(context, "") //!< Legacy single game folder, kept in sync with the first entry of searchLocations
    private var searchLocations by sharedPreferences(context, "") //!< Every game folder, one URI per line

    /**
     * Every game folder, the legacy [searchLocation] is used if the list was never written (settings from before several folders were supported)
     */
    var searchLocationList : List<Uri>
        get() = searchLocations.ifEmpty { searchLocation }.split('\n').filter { it.isNotBlank() }.distinct().map { Uri.parse(it) }
        set(value) {
            val uris = value.map { it.toString() }.distinct()
            searchLocations = uris.joinToString("\n")
            searchLocation = uris.firstOrNull() ?: ""
        }

    /**
     * Set once the first-run setup screen was completed or skipped, so that it's only shown once
     */
    var setupCompleted by sharedPreferences(context, false)

    // Appearance
    var appTheme by sharedPreferences(context, 2)
    var useMaterialYou by sharedPreferences(context, false)
    var layoutType by sharedPreferences(context, 1)
    var sortAppsBy by sharedPreferences(context, 0)
    var selectAction by sharedPreferences(context, false)
    var filterInvalidFiles by sharedPreferences(context, false)

    // Input
    var onScreenControl by sharedPreferences(context, true)
    var onScreenControlFeedback by sharedPreferences(context, true)
    var onScreenControlRecenterSticks by sharedPreferences(context, true)
    var onScreenControlSnapToGrid by sharedPreferences(context, false)
    var onScreenControlUseStickRegions by sharedPreferences(context, false)

    // Other
    var refreshRequired by sharedPreferences(context, false)
}
