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
 * Driving screen: binds [DriveController] with the stick input on [onEnter] and
 * collects [ConnectionManager] aliveness plus [TelemetryHub] telemetry.
 * Owns no loops — bind/collect only; entry and exit are explicit via
 * [onEnter] plus [requestExit] / [confirmExit] / [dismissExit].
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
     * Screen entry: one `MANUAL_TAKE` then full-rate input frames.
     * Idempotent until exit — repeat appearances while bound are no-ops.
     */
    fun onEnter() {
        if (bound) return
        bound = true
        drive.bind { _state.value.throttle to _state.value.steering }
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
     * Confirmed leave: best-effort `MANUAL_RELEASE` via [DriveController]
     * then back to idle heartbeats; the shell navigates on this call.
     */
    fun confirmExit() {
        if (bound) {
            bound = false
            drive.unbind()
        }
        _state.update { it.copy(pendingExit = false) }
    }

    override fun onCleared() {
        if (bound) {
            bound = false
            drive.unbind()
        }
        super.onCleared()
    }

    private companion object {
        const val STICK_SNAP = 0.02f
    }
}
