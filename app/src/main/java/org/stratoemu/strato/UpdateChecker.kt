// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project

package org.stratoemu.strato

import android.util.Log
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

/**
 * @brief Result of a successful update check where a newer release than the currently
 *        installed one was found on GitHub.
 */
data class UpdateInfo(
    val tagName : String,
    val releaseUrl : String,
    val apkDownloadUrl : String?
)

/**
 * @brief Queries the GitHub Releases API for the given repository and compares the latest
 *        published release's date-based tag (e.g. "2026.08.09") against the date embedded
 *        in the currently installed build's version name.
 *
 * The release workflow is expected to tag releases as "YYYY.MM.DD". Comparing these directly
 * as strings already sorts correctly chronologically, but we still extract them into a plain
 * YYYYMMDD integer for a clearer, format-tolerant comparison (also correctly ignores any
 * "git describe" suffix like "-3-gabc1234" that may be appended to the local version name).
 *
 * This never throws: any network failure, malformed response, or parsing error results in
 * `null` being returned so that a failed check is silently ignored rather than shown to
 * the user or crashing the app, matching the "no popup unless there's an update" requirement.
 */
object UpdateChecker {
    private const val TAG = "UpdateChecker"
    private const val TIMEOUT_MS = 8000
    private val DATE_TAG_REGEX = Regex("""(\d{4})[.\-](\d{2})[.\-](\d{2})""")

    /**
     * @param owner The GitHub username/organization that owns the repository (e.g. "67gh")
     * @param repo The repository name (e.g. "strato")
     * @param currentVersionName The version name of the currently installed build, normally
     *                           [org.stratoemu.strato.BuildConfig.VERSION_NAME]. If it doesn't
     *                           contain a parseable date (e.g. a "0.0.0" dev/untagged build),
     *                           any valid remote release is treated as newer.
     * @return Information about the newer release if one exists, otherwise `null`
     */
    fun checkForUpdate(owner : String, repo : String, currentVersionName : String) : UpdateInfo? {
        return try {
            val url = URL("https://api.github.com/repos/$owner/$repo/releases/latest")
            val connection = url.openConnection() as HttpURLConnection
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty("Accept", "application/vnd.github+json")

            try {
                if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                    Log.w(TAG, "GitHub API returned HTTP ${connection.responseCode}, skipping update check")
                    return null
                }

                val body = connection.inputStream.bufferedReader().use { it.readText() }
                val json = JSONObject(body)

                val tagName = json.optString("tag_name", "")
                val remoteDateKey = parseDateKey(tagName) ?: run {
                    Log.w(TAG, "Could not parse a date from release tag '$tagName', skipping update check")
                    return null
                }
                val localDateKey = parseDateKey(currentVersionName) ?: 0L // Untagged/dev build: always offer the update

                if (remoteDateKey <= localDateKey)
                    return null // Already up to date

                val releaseUrl = json.optString("html_url", "https://github.com/$owner/$repo/releases/latest")

                var apkDownloadUrl : String? = null
                val assets = json.optJSONArray("assets")
                if (assets != null) {
                    for (i in 0 until assets.length()) {
                        val asset = assets.getJSONObject(i)
                        val name = asset.optString("name", "")
                        if (name.endsWith(".apk", ignoreCase = true)) {
                            apkDownloadUrl = asset.optString("browser_download_url", null)
                            break
                        }
                    }
                }

                UpdateInfo(tagName, releaseUrl, apkDownloadUrl)
            } finally {
                connection.disconnect()
            }
        } catch (e : Exception) {
            // Deliberately broad: no network, DNS failure, malformed JSON, GitHub API rate
            // limiting, etc. should all just mean "couldn't check right now", never a crash
            // or a wrongly-shown popup.
            Log.w(TAG, "Update check failed, ignoring: ${e.message}")
            null
        }
    }

    /**
     * @brief Extracts a "YYYY.MM.DD"-style date found anywhere in the given string into a
     *        directly comparable YYYYMMDD integer (e.g. "2026.08.09" -> 20260809L). Works
     *        just as well on a bare release tag ("2026.08.09") as on a git-describe-style
     *        version name that has extra suffixes ("2026.08.09-3-gabc1234").
     */
    private fun parseDateKey(version : String) : Long? {
        val match = DATE_TAG_REGEX.find(version) ?: return null
        val (year, month, day) = match.destructured
        return "$year$month$day".toLongOrNull()
    }
}
