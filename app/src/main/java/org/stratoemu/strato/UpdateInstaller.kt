// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.util.Log
import android.widget.Toast
import org.stratoemu.strato.updater.UpdateActivity
import org.stratoemu.strato.updater.APKInstaller

/** Compatibility entry point for MainActivity and the About screen. */
object UpdateInstaller {
    fun downloadAndInstall(context: Context, downloadUrl: String, displayName: String, isArchive: Boolean) {
        try {
            context.startActivity(Intent(context, UpdateActivity::class.java).apply {
                putExtra(UpdateActivity.EXTRA_SOURCE_VERSION, APKInstaller.installedVersion(context))
                putExtra(UpdateActivity.EXTRA_URL, downloadUrl)
                putExtra(UpdateActivity.EXTRA_LABEL, displayName)
                putExtra(UpdateActivity.EXTRA_ARCHIVE, isArchive)
                addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                if (context !is Activity) addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            })
        } catch (e: Exception) {
            Log.w("StratoUpdater", "Cannot open update screen", e)
            Toast.makeText(context, R.string.update_download_failed, Toast.LENGTH_LONG).show()
        }
    }
}
