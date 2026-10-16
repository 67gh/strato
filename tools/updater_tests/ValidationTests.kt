// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.updater

private fun rejected(reason: APKValidationFailure, action: () -> Unit) {
    val error = try { action(); error("Expected rejection: $reason") }
        catch (e: APKValidationException) { e }
    check(error.reason == reason)
    println("PASS rejection: $reason; ${error.message}")
}

fun main() {
    val installed = APKIdentity("org.stratoemu.strato", 42L)
    check(APKValidation.requireCompatible(installed, installed.copy(versionCode = 43)) == 43L)
    check(APKValidation.requireCompatible(installed, installed.copy()) == 42L)
    println("PASS: newer APK and same-code rebuild accepted")
    rejected(APKValidationFailure.OLDER_VERSION) {
        APKValidation.requireCompatible(installed, installed.copy(versionCode = 41))
    }
    rejected(APKValidationFailure.DIFFERENT_PACKAGE) {
        APKValidation.requireCompatible(installed, APKIdentity("org.stratoemu.strato.dev", 100))
    }
    rejected(APKValidationFailure.UNREADABLE) {
        APKValidation.requireCompatible(installed, null)
    }
    val large = installed.copy(versionCode = 0x1_0000_0000L)
    check(APKValidation.requireCompatible(large, large.copy(versionCode = large.versionCode + 1)) == large.versionCode + 1)
    rejected(APKValidationFailure.OLDER_VERSION) {
        APKValidation.requireCompatible(large, installed)
    }
    println("PASS: long version codes are not truncated")

    val current = InstalledAPK(installed, 2000L)
    check(!APKValidation.wasReplaced(current, 42, 2000)) // installer opened or cancelled
    check(APKValidation.wasReplaced(current, 42, 1000)) // same-code replacement
    check(!APKValidation.wasReplaced(current, 42, 0)) // legacy Intent: unknown date
    check(APKValidation.wasReplaced(current, 41, 0)) // legacy Intent: higher code
    check(!APKValidation.wasReplaced(current, 43, 1000)) // no success on downgrade
    check(!APKValidation.wasReplaced(current, 42, 3000)) // timestamp did not advance
    check(!APKValidation.wasReplaced(current, Long.MAX_VALUE, 0)) // absent source metadata
    println("PASS: replacement detection; same code; legacy Intent; no false success on cancel")
}
