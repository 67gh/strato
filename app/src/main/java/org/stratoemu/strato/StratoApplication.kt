/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package org.stratoemu.strato

import android.app.Application
import android.content.Context
import com.google.android.material.color.DynamicColors
import com.google.android.material.color.DynamicColorsOptions
import dagger.hilt.android.HiltAndroidApp
import org.stratoemu.strato.di.getSettings
import org.stratoemu.strato.diagnostics.DiagnosticSession
import java.io.File

/**
 * @return The optimal directory for putting public files inside, this may return a private directory if a public directory cannot be retrieved
 */
fun Context.getPublicFilesDir() : File = getExternalFilesDir(null) ?: filesDir

@HiltAndroidApp
class StratoApplication : Application() {
    init {
        instance = this
    }

    companion object {
        lateinit var instance : StratoApplication
            private set

        val context : Context get() = instance.applicationContext
    }

    override fun onCreate() {
        super.onCreate()
        instance = this

        // Writes any uncaught exception to crash.log in the public files directory, then lets Android handle the crash as usual
        val previousHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                DiagnosticSession.recordCurrentCrash(throwable)
            } catch (_ : Throwable) {
            }
            try {
                File(getPublicFilesDir(), "crash.log").appendText("=== ${java.util.Date()} thread '${thread.name}'\n${throwable.stackTraceToString()}\n")
            } catch (_ : Throwable) {
            }
            previousHandler?.uncaughtException(thread, throwable)
        }

        System.loadLibrary("skyline")

        val dynamicColorsOptions = DynamicColorsOptions.Builder().setPrecondition { _, _ -> getSettings().useMaterialYou }.build()
        DynamicColors.applyToActivitiesIfAvailable(this, dynamicColorsOptions)
    }
}
