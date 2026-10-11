// SPDX-License-Identifier: MPL-2.0
// Copyright © 2024 Strato Revival Project

package org.stratoemu.strato

import android.util.Log
import org.json.JSONObject
import java.io.IOException
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
    val downloadUrl : String?,
    val downloadIsArchive : Boolean
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
 * version. Comparing the release's `published_at` timestamp against [currentBuildTimeMs]
 * is therefore the reliable way to answer "is there a build newer than the APK installed here?".
 *
 * Failures are returned separately from a successful check with no update. Callers can
 * keep automatic checks silent while reporting failures of a manual check.
 */
object UpdateChecker {
    private const val TAG = "UpdateChecker"
    private const val TIMEOUT_MS = 8000

    /**
     * @param owner The GitHub username/organization that owns the repository (e.g. "67gh")
     * @param repo The repository name (e.g. "strato")
     * @param currentBuildTimeMs Epoch milliseconds of the commit used to build the currently
     *                            running APK
     * @param currentBuildCommitFull Full git commit hash embedded in the APK
     * @return Success containing a newer release or null, or a failed check
     */
    fun checkForUpdate(owner : String, repo : String, currentBuildTimeMs : Long, currentBuildCommitFull : String) : Result<UpdateInfo?> {
        return try {
            val url = URL("https://api.github.com/repos/$owner/$repo/releases?per_page=20")
            val connection = url.openConnection() as HttpURLConnection
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.setRequestProperty("Accept", "application/vnd.github+json")

            try {
                if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                    throw IOException("GitHub API returned HTTP ${connection.responseCode}")
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

                val release = newestRelease ?: run {
                    if (releases.length() != 0)
                        throw IOException("No usable published release in response")
                    return Result.success(null)
                }
                if (newestReleaseTimeMs <= currentBuildTimeMs)
                    return Result.success(null) // Already up to date (or a newer local build)

                val tagName = release.optString("tag_name", "")
                if (tagName.isEmpty())
                    throw IOException("Release has no tag name")

                // The release workflow tags the exact commit as vYYYY.MM.DD-<12-char-sha>.
                // The APK stores the full commit hash. Comparing the tag's commit prefix
                // against that full hash avoids treating the exact same build as an update,
                // even if the release is published after the APK was built.
                val releaseCommit = tagName.substringAfterLast('-', "")
                if (releaseCommit.isNotEmpty() && currentBuildCommitFull.startsWith(releaseCommit))
                    return Result.success(null)

                val releaseUrl = release.optString(
                    "html_url",
                    "https://github.com/$owner/$repo/releases/latest"
                )

                var apkDownloadUrl : String? = null
                var zipDownloadUrl : String? = null
                val assets = release.optJSONArray("assets")
                if (assets != null) {
                    for (i in 0 until assets.length()) {
                        val asset = assets.getJSONObject(i)
                        val name = asset.optString("name", "")
                        val url = asset.optString("browser_download_url", null)
                        when {
                            name.equals("strato-update.zip", ignoreCase = true) && zipDownloadUrl == null -> zipDownloadUrl = url
                            name.endsWith(".apk", ignoreCase = true) && apkDownloadUrl == null -> apkDownloadUrl = url
                        }
                    }
                }

                val downloadUrl = zipDownloadUrl ?: apkDownloadUrl
                Result.success(UpdateInfo(tagName, releaseUrl, downloadUrl, zipDownloadUrl != null))
            } finally {
                connection.disconnect()
            }
        } catch (e : Exception) {
            Log.w(TAG, "Update check failed: ${e.message}")
            Result.failure(e)
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
