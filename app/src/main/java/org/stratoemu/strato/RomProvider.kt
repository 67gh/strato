package org.stratoemu.strato

import android.annotation.SuppressLint
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import dagger.hilt.android.qualifiers.ApplicationContext
import org.stratoemu.strato.loader.AppEntry
import org.stratoemu.strato.loader.RomFile
import org.stratoemu.strato.loader.RomFormat
import org.stratoemu.strato.loader.RomFormat.*
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class RomProvider @Inject constructor(@ApplicationContext private val context : Context) {

    companion object {
        private const val TAG = "RomProvider"

        private val ROM_FORMATS = mapOf(
            "nro" to NRO,
            "nso" to NSO,
            "nca" to NCA,
            "nsp" to NSP,
            "xci" to XCI,
        )
    }

    /**
     * Recursively scans [directoryUri] for ROM files using a ContentResolver batch query
     * instead of DocumentFile.listFiles() per entry.
     *
     * This approach:
     * - Supports arbitrarily deep subdirectory trees without URI length limits
     * - Makes a single ContentProvider query per directory instead of one per file
     * - Is significantly faster on large game libraries (100+ files, deeply nested)
     *
     * Android's SAF (Storage Access Framework) ContentProvider is queried directly
     * via [DocumentsContract], which avoids the [DocumentFile] overhead that causes
     * slowdowns and failures on long paths.
     */
    @SuppressLint("DefaultLocale")
    private fun scanDirectory(
        directoryUri : Uri,
        entries : ArrayList<AppEntry>,
        systemLanguage : Int,
        depth : Int = 0
    ) {
        if (depth > 16) {
            // Safety cap — avoids stack overflow on pathological symlink loops
            Log.w(TAG, "Maximum directory depth reached at: $directoryUri")
            return
        }

        val childrenUri = try {
            DocumentsContract.buildChildDocumentsUriUsingTree(
                directoryUri,
                DocumentsContract.getTreeDocumentId(directoryUri)
            )
        } catch (e : Exception) {
            // If the URI doesn't support tree queries, fall back to DocumentFile
            Log.w(TAG, "Tree URI not supported for $directoryUri — using DocumentFile fallback")
            fallbackScan(directoryUri, entries, systemLanguage, depth)
            return
        }

        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        )

        try {
            context.contentResolver.query(
                childrenUri, projection, null, null, null
            )?.use { cursor ->
                val idIndex   = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val nameIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                val mimeIndex = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)

                while (cursor.moveToNext()) {
                    val docId    = cursor.getString(idIndex)   ?: continue
                    val name     = cursor.getString(nameIndex) ?: continue
                    val mimeType = cursor.getString(mimeIndex) ?: continue

                    val childUri = DocumentsContract.buildDocumentUriUsingTree(directoryUri, docId)

                    if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) {
                        // Recurse into subdirectory
                        scanDirectory(childUri, entries, systemLanguage, depth + 1)
                    } else {
                        // Check extension
                        val ext = name.substringAfterLast('.', "").lowercase()
                        ROM_FORMATS[ext]?.let { romFormat ->
                            try {
                                entries.add(RomFile(context, romFormat, childUri, systemLanguage).appEntry)
                            } catch (e : Exception) {
                                Log.w(TAG, "Failed to load ROM metadata for $name: ${e.message}")
                            }
                        }
                    }
                }
            }
        } catch (e : Exception) {
            Log.e(TAG, "Error scanning directory $directoryUri: ${e.message}")
        }
    }

    /**
     * Fallback using DocumentFile for URIs that don't support [DocumentsContract] tree queries
     * (e.g. some third-party file pickers).
     */
    @SuppressLint("DefaultLocale")
    private fun fallbackScan(
        directoryUri : Uri,
        entries : ArrayList<AppEntry>,
        systemLanguage : Int,
        depth : Int
    ) {
        if (depth > 16) return
        val dir = DocumentFile.fromTreeUri(context, directoryUri) ?: return
        dir.listFiles().forEach { file ->
            when {
                file.isDirectory -> fallbackScan(file.uri, entries, systemLanguage, depth + 1)
                else -> {
                    val ext = file.name?.substringAfterLast('.')?.lowercase() ?: return@forEach
                    ROM_FORMATS[ext]?.let { romFormat ->
                        try {
                            entries.add(RomFile(context, romFormat, file.uri, systemLanguage).appEntry)
                        } catch (e : Exception) {
                            Log.w(TAG, "Failed to load ROM metadata for ${file.name}: ${e.message}")
                        }
                    }
                }
            }
        }
    }

    fun loadRoms(searchLocation : Uri, systemLanguage : Int) : ArrayList<AppEntry> {
        val entries = ArrayList<AppEntry>()
        scanDirectory(searchLocation, entries, systemLanguage)
        return entries
    }
}
