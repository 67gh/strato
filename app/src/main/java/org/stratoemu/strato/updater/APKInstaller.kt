// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.core.content.FileProvider
import java.io.File
import java.io.IOException

object APKInstaller {
    @Suppress("DEPRECATION")
    fun installedVersion(context: Context): Long =
        context.packageManager.getPackageInfo(context.packageName, 0).longVersionCode

    @Suppress("DEPRECATION")
    fun validate(context: Context, apk: File): Long {
        if (!apk.isFile || apk.length() == 0L) throw IOException("APK missing or empty")
        val candidate = context.packageManager.getPackageArchiveInfo(apk.path, 0)
            ?: throw IOException("Downloaded file is not a readable APK")
        if (candidate.packageName != context.packageName)
            throw IOException("APK belongs to a different application or build variant")
        if (candidate.longVersionCode <= installedVersion(context))
            throw IOException("APK version is not newer than the installed application")
        return candidate.longVersionCode
    }

    fun permissionIntent(context: Context) = Intent(
        Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${context.packageName}")
    )

    fun installIntent(context: Context, apk: File): Intent {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", apk)
        return Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/vnd.android.package-archive")
            clipData = ClipData.newRawUri("Strato update", uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
    }
}
