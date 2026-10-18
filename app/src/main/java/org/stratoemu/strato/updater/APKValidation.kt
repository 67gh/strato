// SPDX-License-Identifier: MPL-2.0
// Copyright © 2026 Strato contributors

package org.stratoemu.strato.updater

import java.io.IOException

data class APKIdentity(val packageName: String, val versionCode: Long)
data class InstalledAPK(val identity: APKIdentity, val lastUpdateTime: Long)

enum class APKValidationFailure { UNREADABLE, DIFFERENT_PACKAGE, OLDER_VERSION }

class APKValidationException(
    val reason: APKValidationFailure,
    val installed: APKIdentity? = null,
    val candidate: APKIdentity? = null
) : IOException("APK validation: $reason; installed=$installed; downloaded=$candidate")

/** Package metadata comes from Android's APK parser, not from the release name. */
object APKValidation {
    fun requireCompatible(installed: APKIdentity, candidate: APKIdentity?): Long {
        if (candidate == null)
            throw APKValidationException(APKValidationFailure.UNREADABLE, installed)
        if (candidate.packageName != installed.packageName)
            throw APKValidationException(APKValidationFailure.DIFFERENT_PACKAGE, installed, candidate)
        // Android can replace an app with the same versionCode. This is common
        // for rebuilt releases; only a lower code is a downgrade.
        if (candidate.versionCode < installed.versionCode)
            throw APKValidationException(APKValidationFailure.OLDER_VERSION, installed, candidate)
        return candidate.versionCode
    }

    fun wasReplaced(installed: InstalledAPK, sourceVersion: Long, sourceUpdateTime: Long): Boolean =
        installed.identity.versionCode > sourceVersion ||
            (installed.identity.versionCode == sourceVersion && sourceUpdateTime > 0 &&
                installed.lastUpdateTime > sourceUpdateTime)
}
