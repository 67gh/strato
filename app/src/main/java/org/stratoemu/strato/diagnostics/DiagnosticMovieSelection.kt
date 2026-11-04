// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.diagnostics

import android.content.Context
import android.content.Intent
import android.net.Uri
import java.io.File
import java.io.IOException

/** The menu and emulator may run in separate processes. Publish the URI by atomic rename. */
internal object DiagnosticMovieSelection {
    private fun selection(context: Context) = File(context.noBackupFilesDir, "diagnostic-test-movie")

    fun read(context: Context): Uri? = try {
        val file = selection(context)
        if (file.length() in 1L..16384L) Uri.parse(file.readText()).takeIf { it.scheme == "content" } else null
    } catch (e: IOException) { null }

    @Synchronized fun save(context: Context, uri: Uri): Boolean {
        if (uri.scheme != "content" || uri.toString().toByteArray().size > 16384) return false
        var temp: File? = null
        return try {
            context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            val target = selection(context)
            val staged = File.createTempFile("diagnostic-movie-", ".tmp", target.parentFile)
            temp = staged
            staged.writeText(uri.toString())
            staged.renameTo(target)
        } catch (e: IOException) { false }
          catch (e: SecurityException) { false }
        finally { temp?.delete() }
    }
}
