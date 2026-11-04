// SPDX-License-Identifier: MPL-2.0
package org.stratoemu.strato.emulation

import android.app.Activity
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaPlayer
import android.net.Uri
import android.os.CancellationSignal
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.Surface
import android.view.TextureView
import android.widget.Button
import android.widget.FrameLayout
import java.util.concurrent.atomic.AtomicBoolean

/** Diagnostic playback. All guest state and the Vulkan Surface belong to the emulator. */
class AndroidVideoOverlay(private val activity: Activity, private val host: FrameLayout) {
    enum class Result { COMPLETED, SKIPPED, FAILED, CANCELLED }
    private enum class State { IDLE, PREPARING, PLAYING, COMPLETING, FAILED, CANCELLED, RESTORING }

    private val ui = Handler(Looper.getMainLooper())
    private var active: Session? = null

    fun play(uri: Uri, completion: (Result) -> Unit): Boolean {
        if (active != null || activity.isFinishing || activity.isDestroyed) return false
        Session(uri, completion).also { active = it }.attach()
        return true
    }

    fun stop() { active?.finish(Result.CANCELLED) }

    private inner class Session(val uri: Uri, val completion: (Result) -> Unit) :
        TextureView.SurfaceTextureListener {
        val layer = FrameLayout(activity)
        val texture = TextureView(activity)
        val thread = HandlerThread("StratoMp4").apply { start() }
        val worker = Handler(thread.looper)
        val closed = AtomicBoolean(false)
        val cancel = CancellationSignal()
        // Only the playback looper touches player and surface.
        var player: MediaPlayer? = null
        var surface: Surface? = null
        var state = State.IDLE
        val started = SystemClock.elapsedRealtime()
        var videoWidth = 0
        var videoHeight = 0
        val timeout = Runnable { fail("timeout") }

        fun transition(next: State) {
            Log.i("StratoMp4", "manual $state -> $next elapsedMs=${SystemClock.elapsedRealtime() - started}")
            state = next
        }

        fun attach() {
            transition(State.PREPARING)
            layer.setBackgroundColor(Color.BLACK)
            layer.isClickable = true
            texture.surfaceTextureListener = this
            layer.addView(texture, FrameLayout.LayoutParams(-1, -1))
            val skip = Button(activity).apply {
                text = "Passer"
                setOnClickListener { finish(Result.SKIPPED) }
            }
            layer.addView(skip, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.END))
            ui.postDelayed(timeout, 20_000)
            host.addView(layer, FrameLayout.LayoutParams(-1, -1))
        }

        override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
            if (closed.get()) return
            val target = Surface(st)
            worker.post {
                surface = target
                if (closed.get()) return@post
                try {
                    val media = MediaPlayer()
                    player = media
                    media.setSurface(target)
                    media.setOnVideoSizeChangedListener { _, w, h ->
                        ui.post {
                            if (!closed.get()) {
                                videoWidth = w
                                videoHeight = h
                                fit()
                            }
                        }
                    }
                    media.setOnErrorListener { _, what, extra ->
                        ui.post { fail("MediaPlayer what=$what extra=$extra") }
                        true
                    }
                    media.setOnCompletionListener { ui.post { finish(Result.COMPLETED) } }
                    media.setOnPreparedListener {
                        if (!closed.get()) {
                            try {
                                val duration = media.duration
                                require(duration > 0)
                                media.start()
                                ui.post {
                                    if (!closed.get()) {
                                        Log.i("StratoMp4", "durationMs=$duration")
                                        transition(State.PLAYING)
                                        ui.removeCallbacks(timeout)
                                        ui.postDelayed(timeout, duration.toLong() + 30_000)
                                    }
                                }
                            } catch (e: Exception) { ui.post { fail("start ${e.javaClass.simpleName}") } }
                        }
                    }
                    val descriptor = activity.contentResolver.openAssetFileDescriptor(uri, "r", cancel)
                        ?: throw java.io.IOException("missing descriptor")
                    descriptor.use { fd ->
                        val extractor = MediaExtractor()
                        try {
                            extractor.setDataSource(fd)
                            val tracks = (0 until extractor.trackCount).mapNotNull { index ->
                                extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME)
                            }
                            require(tracks.any { it.startsWith("video/") })
                            Log.i("StratoMp4", "tracks=${tracks.joinToString()}")
                        } finally {
                            extractor.release()
                        }
                        if (!closed.get()) media.setDataSource(fd)
                    }
                    if (!closed.get()) media.prepareAsync()
                } catch (e: Exception) { ui.post { fail("prepare ${e.javaClass.simpleName}") } }
            }
        }

        fun fit() {
            if (videoWidth <= 0 || videoHeight <= 0 || texture.width == 0 || texture.height == 0) return
            val ratio = videoWidth.toFloat() * texture.height / (videoHeight.toFloat() * texture.width)
            texture.setTransform(Matrix().apply {
                setScale(minOf(1f, ratio), minOf(1f, 1f / ratio), texture.width / 2f, texture.height / 2f)
            })
        }
        override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) = fit()
        override fun onSurfaceTextureUpdated(st: SurfaceTexture) = Unit
        override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
            finish(Result.CANCELLED)
            return true
        }

        fun fail(reason: String) {
            if (!closed.get()) Log.w("StratoMp4", "manual playback failed: $reason")
            finish(Result.FAILED)
        }

        fun finish(result: Result) {
            if (!closed.compareAndSet(false, true)) return
            transition(when (result) {
                Result.FAILED -> State.FAILED
                Result.CANCELLED -> State.CANCELLED
                else -> State.COMPLETING
            })
            ui.removeCallbacks(timeout)
            texture.surfaceTextureListener = null
            transition(State.RESTORING)
            host.removeView(layer)
            // Signal a slow document provider independently of its blocked playback looper.
            java.util.concurrent.ForkJoinPool.commonPool().execute { cancel.cancel() }
            worker.post {
                try {
                    player?.release()
                } catch (e: RuntimeException) {
                    Log.w("StratoMp4", "release ${e.javaClass.simpleName}")
                } finally {
                    player = null
                    surface?.release()
                    surface = null
                    thread.quitSafely()
                    ui.post {
                        transition(State.IDLE)
                        active = null
                        completion(result)
                    }
                }
            }
        }
    }
}
