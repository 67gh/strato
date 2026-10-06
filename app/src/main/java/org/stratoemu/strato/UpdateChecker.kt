// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project

package org.stratoemu.strato

import android.util.Log
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

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
 * @brief Queries the GitHub Releases API for the given repository and compares the release's
 *        publish timestamp against the currently installed APK's install/update timestamp.
 *
 * The repository publishes its builds as GitHub pre-releases, so the GitHub
 * `releases/latest` endpoint is not sufficient: it can ignore the newest build entirely.
 * We therefore query the release list and select the newest published release, including
 * pre-releases.
 *
 * Release tags are generated from a date and commit hash, so they are not a stable semantic
 * version. Comparing the release's `published_at` timestamp against [currentInstallTimeMs]
 * is therefore the reliable way to answer "is there a build newer than the APK installed here?".
 *
 * This never throws: any network failure, malformed response, or parsing error results in
 * `null` being returned so that a failed check is silently ignored rather than shown to
 * the user or crashing the app, matching the "no popup unless there's an update" requirement.
 */
object UpdateChecker {
    private const val TAG = "UpdateChecker"
    private const val TIMEOUT_MS = 8000

    /**
     * @param owner The GitHub username/organization that owns the repository (e.g. "67gh")
     * @param repo The repository name (e.g. "strato")
     * @param currentBuildTimeMs Epoch milliseconds of the commit used to build the currently
     *                            running APK
     * @param currentBuildCommitShort Short git commit hash embedded in the APK
     * @return Information about the newer release if one exists, otherwise `null`
     */
    fun checkForUpdate(owner : String, repo : String, currentBuildTimeMs : Long, currentBuildCommitShort : String) : UpdateInfo? {
        return try {
            val url = URL("https://api.github.com/repos/$owner/$repo/releases?per_page=20")
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
                val releases = org.json.JSONArray(body)

                var newestRelease : JSONObject? = null
                var newestReleaseTimeMs = Long.MIN_VALUE

                for (i in 0 until releases.length()) {
                    val release = releases.optJSONObject(i) ?: continue
                    if (release.optBoolean("draft", false))
                        continue

                    val publishedAt = release.optString("published_at", "")
                    val releaseTimeMs = parseIso8601(publishedAt) ?: continue
                    if (releaseTimeMs > newestReleaseTimeMs) {
                        newestReleaseTimeMs = releaseTimeMs
                        newestRelease = release
                    }
                }

                val release = newestRelease ?: return null
                if (newestReleaseTimeMs <= currentBuildTimeMs)
                    return null // Already up to date (or somehow newer, e.g. a local dev build)

                val tagName = release.optString("tag_name", "")
                if (tagName.isEmpty())
                    return null

                // The release workflow tags the exact commit as vYYYY.MM.DD-<short-sha>.
                // A release can therefore be published after its APK was built; matching the
                // commit avoids offering the exact same build as an "update".
                if (tagName.endsWith("-${currentBuildCommitShort}"))
                    return null

                val releaseUrl = release.optString(
                    "html_url",
                    "https://github.com/$owner/$repo/releases/latest"
                )

                var apkDownloadUrl : String? = null
                val assets = release.optJSONArray("assets")
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
     * @brief Parses a GitHub API ISO-8601 UTC timestamp (e.g. "2026-08-09T14:03:21Z") into
     *        epoch milliseconds, or null if it doesn't match the expected format.
     */
    private fun parseIso8601(value : String) : Long? {
        if (value.isEmpty()) return null
        return try {
            val format = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US)
            format.timeZone = TimeZone.getTimeZone("UTC")
            format.parse(value)?.time
        } catch (e : Exception) {
            null
        }
    }
}
