package com.desky.shared.features.home

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.Telemetry
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.control.ConnectionManager
import com.desky.shared.features.control.TelemetryHub
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

/** One home telemetry pill: label plus value derived from real link data. */
data class HomePill(
    val label: String,
    val value: String,
)

/**
 * Pure pill mapping: Link is Online only while Connected, else Offline;
 * Status comes from `driving` plus the cliff flag. Missing telemetry
 * renders as an em dash, never a fabricated zero.
 */
fun mapHomePills(aliveness: Aliveness, telemetry: Telemetry?): List<HomePill> {
    val link = if (aliveness == Aliveness.Connected) "Online" else "Offline"
    val status = telemetry?.let {
        (if (it.driving) "Driving" else "Idle") + if (it.cliff) " · Cliff" else ""
    } ?: "—"
    return listOf(
        HomePill("Link", link),
        HomePill("Status", status),
    )
}

/** Home-screen state (UDF: single [StateFlow], no intents — entries are static). */
data class HomeUiState(
    val pairedDevice: PairedDevice? = null,
    val aliveness: Aliveness = Aliveness.Unpaired,
    val telemetry: Telemetry? = null,
    val entries: List<HomeEntry> = HomeEntry.entries,
) {
    /** Derived pills for the shell; always consistent with [aliveness] plus [telemetry]. */
    val pills: List<HomePill>
        get() = mapHomePills(aliveness, telemetry)
}

/**
 * Post-pairing landing: mirrors the current pairing plus [ConnectionManager]
 * aliveness plus the latest [TelemetryHub] snapshot.
 * Owns no loops — all flows are collected, never started here.
 */
class HomeViewModel(
    deviceRepo: DeviceRepository,
    connectionManager: ConnectionManager,
    telemetryHub: TelemetryHub,
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
        viewModelScope.launch {
            telemetryHub.telemetry.collect { telemetry ->
                _state.update { it.copy(telemetry = telemetry) }
            }
        }
    }
}
