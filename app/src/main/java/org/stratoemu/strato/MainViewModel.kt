package org.stratoemu.strato

import android.app.Application
import android.content.Context
import android.net.Uri
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import org.stratoemu.strato.loader.AppEntry
import org.stratoemu.strato.utils.fromFile
import org.stratoemu.strato.utils.toFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import javax.inject.Inject

sealed class MainState {
    object Loading : MainState()
    class Loaded(val items : ArrayList<AppEntry>) : MainState()
    class Error(val ex : Exception) : MainState()
}

@HiltViewModel
class MainViewModel @Inject constructor(@ApplicationContext context : Context, private val romProvider : RomProvider) : AndroidViewModel(context as Application) {
    companion object {
        private val TAG = MainViewModel::class.java.simpleName
    }

    private var state
        get() = _stateData.value
        set(value) { _stateData.value = value }
    private val _stateData = MutableLiveData<MainState>()
    val stateData : LiveData<MainState> = _stateData

    /**
     * This refreshes the contents of the adapter by either trying to load cached adapter data or searches for them to recreate a list
     *
     * @param loadFromFile If this is false then trying to load cached adapter data is skipped entirely
     */
    fun loadRoms(context : Context, loadFromFile : Boolean, searchLocations : List<Uri>, systemLanguage : Int) {
        if (state == MainState.Loading)
            return
        state = MainState.Loading

        val previousRefresh = refreshJob
        previousRefresh?.cancel()
        val applicationContext = context.applicationContext
        viewModelScope.launch {
            try {
                // Let a cancelled scan finish its blocking file work before replacing its cache.
                previousRefresh?.join()
                val romElements = withContext(Dispatchers.IO) {
                    val romsFile = File(getApplication<StratoApplication>().filesDir, "roms.bin")
                    if (loadFromFile && romsFile.exists()) {
                        try {
                            return@withContext fromFile<ArrayList<AppEntry>>(romsFile)
                        } catch (e : Exception) {
                            Log.w(TAG, "Ran into exception while loading: ${e.message}")
                        }
                    }
                    searchLocations.forEach { location ->
                        try {
                            KeyReader.importFromLocation(applicationContext, location)
                        } catch (e : Exception) {
                            Log.w(TAG, "Couldn't look for keys in '$location': ${e.message}")
                        }
                    }
                    romProvider.loadRoms(searchLocations, systemLanguage).also { it.toFile(romsFile) }
                }
                state = MainState.Loaded(romElements)
                if (loadFromFile) checkRomHash(searchLocations, systemLanguage)
            } catch (e : CancellationException) {
                throw e
            } catch (e : Exception) {
                Log.w(TAG, "Ran into exception while loading games: ${e.message}")
                state = MainState.Error(e)
            }
        }
    }

    /**
     * Tracks whether an auto refresh is already in progress
     */
    private var refreshJob : Job? = null

    /**
     * This checks if the roms have changed since the last time they were loaded and if so it reloads them
     */
    fun checkRomHash(searchLocations : List<Uri>, systemLanguage : Int) {
        // Skip if an auto refresh is already in progress or if the state hasn't already loaded
        if (refreshJob?.isActive == true)
            return
        val currentState = state as? MainState.Loaded ?: return
        refreshJob = viewModelScope.launch {
            try {
                val romElements = withContext(Dispatchers.IO) {
                    romProvider.loadRoms(searchLocations, systemLanguage).also {
                        if (it != currentState.items)
                            it.toFile(File(getApplication<StratoApplication>().filesDir, "roms.bin"))
                    }
                }
                if (romElements != currentState.items)
                    state = MainState.Loaded(romElements)
            } catch (e : CancellationException) {
                throw e
            } catch (e : Exception) {
                // Keep the already displayed library when a background refresh fails.
                Log.w(TAG, "Couldn't refresh games: ${e.message}")
            }
        }
    }
}
