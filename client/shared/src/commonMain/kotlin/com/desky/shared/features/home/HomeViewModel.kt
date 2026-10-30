package com.desky.shared.features.home

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.DeviceRepository
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.control.ConnectionManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Post-pairing landing entries. */
enum class HomeEntry {
    Drive,
    Expressions,
    Settings,
}

/** Home-screen state (UDF: single [StateFlow], no intents — entries are static). */
data class HomeUiState(
    val pairedDevice: PairedDevice? = null,
    val aliveness: Aliveness = Aliveness.Unpaired,
    val entries: List<HomeEntry> = HomeEntry.entries,
)

/**
 * Post-pairing landing: mirrors the current pairing plus [ConnectionManager]
 * aliveness. Owns no loops — both flows are collected, never started here.
 */
class HomeViewModel(
    deviceRepo: DeviceRepository,
    connectionManager: ConnectionManager,
) : ViewModel() {

    private val _state = MutableStateFlow(HomeUiState())
    val state: StateFlow<HomeUiState> = _state.asStateFlow()

    init {
        viewModelScope.launch {
            deviceRepo.observePaired().collect { device ->
                _state.update { it.copy(pairedDevice = device) }
            }
        }
        viewModelScope.launch {
            connectionManager.aliveness.collect { aliveness ->
                _state.update { it.copy(aliveness = aliveness) }
            }
        }
    }
}
