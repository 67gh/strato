/* SPDX-License-Identifier: MPL-2.0 */
package org.stratoemu.strato.utils
import java.io.File
import java.util.Locale
object DriverPackageValidation {
    fun safeComponent(value : String) = value.isNotBlank() && value != "." && value != ".." &&
        !value.any { it == '/' || it == '\\' || it == '\u0000' }
    fun family(name : String, vendor : String, library : String, target : String) : String {
        if (target.isNotEmpty()) return target.lowercase(Locale.ROOT)
        val text = "$name $vendor $library".lowercase(Locale.ROOT)
        return when {
            listOf("panvk", "panfrost", "mali").any { it in text } -> "mali"
            listOf("turnip", "adreno", "qualcomm").any { it in text } -> "adreno"
            else -> "unknown"
        }
    }
    fun compatible(platform : String, family : String) =
        platform == family || (platform == "adreno" && family == "unknown")
    fun arm64SharedLibrary(file : File) : Boolean {
        if (!file.isFile || file.length() < 64) return false
        val h = ByteArray(64)
        file.inputStream().use { input ->
            var count = 0
            while (count < h.size) {
                val n = input.read(h, count, h.size - count)
                if (n <= 0) return false
                count += n
            }
        }
        return h[0] == 0x7f.toByte() && h[1] == 69.toByte() && h[2] == 76.toByte() && h[3] == 70.toByte() &&
            h[4] == 2.toByte() && h[5] == 1.toByte() && h[6] == 1.toByte() &&
            h[16] == 3.toByte() && h[17] == 0.toByte() && h[18] == 183.toByte() && h[19] == 0.toByte() &&
            h[52] == 64.toByte() && h[53] == 0.toByte()
    }
}
