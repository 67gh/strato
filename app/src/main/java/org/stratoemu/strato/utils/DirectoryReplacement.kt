// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.utils

import java.io.File
import java.io.IOException
import java.nio.file.Files

/** Installs a fully prepared directory without deleting the previous copy first. */
object DirectoryReplacement {
    @Synchronized
    fun replace(target : File, prepare : (File) -> Unit) {
        val parent = target.absoluteFile.parentFile ?: throw IOException("Missing parent directory")
        if (!parent.isDirectory && !parent.mkdirs()) throw IOException("Cannot create $parent")
        val backup = File(parent, ".${target.name}.import-backup")
        if (backup.exists()) {
            if (!target.exists()) {
                if (!backup.renameTo(target)) throw IOException("Cannot restore $backup")
            } else if (!backup.deleteRecursively()) {
                throw IOException("Cannot remove previous backup $backup")
            }
        }
        val staging = Files.createTempDirectory(parent.toPath(), ".${target.name}.import-").toFile()
        try {
            prepare(staging)
            val hadPrevious = target.exists()
            if (hadPrevious && !target.renameTo(backup)) throw IOException("Cannot back up $target")
            if (!staging.renameTo(target)) {
                if (hadPrevious && !backup.renameTo(target))
                    throw IOException("Install failed; previous data retained at $backup")
                throw IOException("Cannot install $target")
            }
            // If cleanup fails, leave the backup for the next import to recover/clean.
            if (hadPrevious) backup.deleteRecursively()
        } finally {
            staging.deleteRecursively()
        }
    }
}
