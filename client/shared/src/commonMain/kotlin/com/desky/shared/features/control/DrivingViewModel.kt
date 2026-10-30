package com.desky.shared.features.control

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.core.Telemetry
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlin.math.abs

/** Driving-screen state (UDF: single [StateFlow] + explicit intents below). */
data class DrivingUiState(
    val throttle: Float = 0f,
    val steering: Float = 0f,
    val telemetry: Telemetry? = null,
    val aliveness: Aliveness = Aliveness.Unpaired,
    /**
     * Exit handshake: [requestExit] raises it (shell shows the confirm
     * dialog), [confirmExit] unbinds and clears it (shell navigates away),
     * [dismissExit] clears it (shell stays).
     */
    val pendingExit: Boolean = false,
)

/**
 * Driving screen: binds [DriveController] with the stick input while active
 * and collects [ConnectionManager] aliveness plus [TelemetryHub] telemetry.
 * Owns no loops — bind/collect only; visibility is explicit via [activate]
 * / [deactivate] plus [requestExit] / [confirmExit] / [dismissExit].
 */
class DrivingViewModel(
    private val drive: DriveController,
    connectionManager: ConnectionManager,
    telemetryHub: TelemetryHub,
) : ViewModel() {

    private val _state = MutableStateFlow(DrivingUiState())
    val state: StateFlow<DrivingUiState> = _state.asStateFlow()

    private var bound = false

    init {
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

    /**
     * Screen visible: one `MANUAL_TAKE` then full-rate input frames.
     * Idempotent while bound — repeat activations (foreground return,
     * recomposition) are no-ops. Every [deactivate] re-arms, so the take
     * follows visibility, not navigation.
     */
    fun activate() {
        if (bound) return
        bound = true
        drive.bind { _state.value.throttle to _state.value.steering }
    }

    /**
     * Screen hidden: best-effort `MANUAL_RELEASE` via [DriveController].
     * Idempotent — safe to race navigation-away against backgrounding.
     * Silent by design: no exit confirmation, the shell stays put.
     */
    fun deactivate() {
        if (!bound) return
        bound = false
        drive.unbind()
    }

    /** Stick deflection in -1..1, snapped to center below the drive threshold. */
    fun onStick(throttle: Float, steering: Float) {
        _state.update {
            it.copy(
                throttle = throttle.coerceIn(-1f, 1f).let { v -> if (abs(v) < STICK_SNAP) 0f else v },
                steering = steering.coerceIn(-1f, 1f).let { v -> if (abs(v) < STICK_SNAP) 0f else v },
            )
        }
    }

    fun requestExit() {
        _state.update { it.copy(pendingExit = true) }
    }

    fun dismissExit() {
        _state.update { it.copy(pendingExit = false) }
    }

    /**
     * Confirmed leave: [deactivate] then clear the handshake; the shell
     * navigates on this call.
     */
    fun confirmExit() {
        deactivate()
        _state.update { it.copy(pendingExit = false) }
    }

    override fun onCleared() {
        deactivate()
        super.onCleared()
    }

    private companion object {
        const val STICK_SNAP = 0.02f
    }
}
