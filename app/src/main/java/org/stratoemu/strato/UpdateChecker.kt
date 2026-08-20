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
 * NOTE: this repo's release tags are always "0.0.0-<hash>" (a fixed "0.0.0" prefix followed by
 * a changing alphanumeric suffix), so there is no usable version or date to parse out of the
 * tag name itself - comparing tag strings or extracting a date from them will never work here.
 * Instead we compare the release's "published_at" timestamp (always present and reliable on the
 * GitHub API) against [currentInstallTimeMs], which the caller should obtain via
 * `packageManager.getPackageInfo(packageName, 0).lastUpdateTime` - a genuinely local, reliable
 * signal for "when was the app I'm currently running actually installed/updated".
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
     * @param currentInstallTimeMs Epoch milliseconds of when the currently running APK was
     *                             installed/last updated on this device (see note above)
     * @return Information about the newer release if one exists, otherwise `null`
     */
    fun checkForUpdate(owner : String, repo : String, currentInstallTimeMs : Long) : UpdateInfo? {
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
                val publishedAt = json.optString("published_at", "")
                val remoteTimeMs = parseIso8601(publishedAt) ?: run {
                    Log.w(TAG, "Could not parse published_at '$publishedAt', skipping update check")
                    return null
                }

                if (remoteTimeMs <= currentInstallTimeMs)
                    return null // Already up to date (or somehow newer, e.g. a local dev build)

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
