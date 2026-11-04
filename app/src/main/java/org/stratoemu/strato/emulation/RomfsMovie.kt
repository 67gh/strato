// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.emulation

import android.media.MediaDataSource
import android.os.ParcelFileDescriptor
import androidx.annotation.Keep
import java.io.Closeable
import java.io.IOException

/** The caller owns the ROM descriptor until construction succeeds. */
@Keep
internal class RomfsMovie(private val descriptor: ParcelFileDescriptor, format: Int,
                         keyDirectory: String, position: Long) : Closeable {
    private external fun openNative(fd: Int, format: Int, keys: String, offset: Long): Long
    private external fun sizeNative(handle: Long): Long
    private external fun readNative(handle: Long, position: Long, out: ByteArray, offset: Int, count: Int): Int
    private external fun closeNative(handle: Long)

    private var handle = openNative(descriptor.fd, format, keyDirectory, position)
    init { if (handle == 0L) throw IOException("No complete RomFS movie at the observed offset") }

    // Extractor and player each receive their own closeable view. The session
    // owns the shared reader and closes it after the player releases its view.
    fun view(): MediaDataSource = object : MediaDataSource() {
        override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int =
            synchronized(this@RomfsMovie) {
                if (handle == 0L) throw IOException("Closed ROM")
                val read = readNative(handle, position, buffer, offset, size)
                if (read < -1) throw IOException("ROM read failed")
                read
            }
        override fun getSize(): Long = synchronized(this@RomfsMovie) {
            if (handle == 0L) throw IOException("Closed ROM")
            sizeNative(handle)
        }
        override fun close() = Unit
    }

    @Synchronized override fun close() {
        if (handle == 0L) return
        closeNative(handle)
        handle = 0
        descriptor.close()
    }
}
