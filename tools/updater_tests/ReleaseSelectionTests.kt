// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato

import org.json.JSONArray
import org.json.JSONObject

private var checks = 0
private fun test(name: String, action: () -> Unit) { action(); checks++; println("PASS: $name") }
private fun asset(name: String, url: String = "https://example.test/$name", state: String = "uploaded") =
    JSONObject().put("name", name).put("browser_download_url", url).put("state", state)
private fun release(vararg assets: JSONObject, date: String = "2026-10-15T10:00:00Z", draft: Boolean = false) =
    JSONObject().put("tag_name", "v2026.10.15-abcdef123456").put("published_at", date)
        .put("draft", draft).put("prerelease", true).put("html_url", "https://example.test/releases/one")
        .put("assets", JSONArray(assets.toList()))
private fun select(vararg releases: JSONObject, commit: String = "0123456789012345678901234567890123456789", time: Long = 0) =
    UpdateChecker.parseReleases(JSONArray(releases.toList()).toString(), "owner", "repo", time, commit)

fun main() {
    test("canonical direct APK selected instead of ZIP or another APK") {
        val r = select(release(asset("strato-update.zip"), asset("other.apk"), asset("strato-mainline-release.apk")))!!
        check(r.downloadUrl == "https://example.test/strato-mainline-release.apk" && !r.downloadIsArchive)
    }
    test("old release APK remains usable beside its ZIP") {
        val r = select(release(asset("strato-update.zip"), asset("app-mainline-release.apk")))!!
        check(r.downloadUrl!!.endsWith("app-mainline-release.apk") && !r.downloadIsArchive)
    }
    test("ZIP-only release returns release page without archive download") {
        val r = select(release(asset("strato-update.zip")))!!
        check(r.downloadUrl == null && !r.downloadIsArchive && r.releaseUrl == "https://example.test/releases/one")
    }
    test("incomplete uploads and blank URLs skipped; APK extension case-insensitive") {
        val r = select(release(asset("strato-mainline-release.apk", state = "starter"), asset("empty.apk", ""), asset("APP.APK")))!!
        check(r.downloadUrl!!.endsWith("APP.APK") && !r.downloadIsArchive)
    }
    test("newest published prerelease selected, draft ignored") {
        val r = select(release(asset("draft.apk"), date = "2026-10-17T10:00:00Z", draft = true),
            release(asset("new.apk")), release(asset("old.apk"), date = "2026-10-14T10:00:00Z"))!!
        check(r.downloadUrl!!.endsWith("new.apk"))
    }
    test("same embedded build commit not offered again") {
        check(select(release(asset("new.apk")), commit = "abcdef1234567890123456789012345678901234") == null)
    }
    test("release older than local build not offered") {
        check(select(release(asset("new.apk")), time = Long.MAX_VALUE) == null)
    }
    test("empty release list has no update") { check(select() == null) }
    test("malformed response fails rather than claiming up-to-date") {
        var failed = false
        try { UpdateChecker.parseReleases("not-json", "owner", "repo", 0, "") }
        catch (_: org.json.JSONException) { failed = true }
        check(failed)
    }
    println("$checks release selection groups passed using the production UpdateChecker and Android JSONObject.")
}
