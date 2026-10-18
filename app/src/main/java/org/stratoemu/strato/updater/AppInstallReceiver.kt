// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import java.io.File

/** Only Android's notification that THIS app was replaced is a successful update. */
class AppInstallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return
        val pending = goAsync()
        Thread {
            try {
                File(context.cacheDir, "updates").deleteRecursively()
            } finally {
                pending.finish()
            }
        }.start()
    }
}
