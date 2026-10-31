package com.desky.shared.features.control

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.core.Telemetry
import com.desky.shared.features.session.SessionManager
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
    val error: String? = null,
    /**
     * Latched emergency stop: [onEmergencyStop] raises it (shell shows the
     * release affordance), [onReleaseEmergency] clears it. Held across link
     * loss and screen hides — only an explicit release unlatches.
     */
    val eStopped: Boolean = false,
    /**
     * Exit handshake: [requestExit] raises it (shell shows the confirm
     * dialog), [confirmExit] unbinds and clears it (shell navigates away),
     * [dismissExit] clears it (shell stays).
     */
    val pendingExit: Boolean = false,
)

/**
 * Driving screen: enters manual while visible and collects
 * [ConnectionManager] aliveness plus [TelemetryHub] telemetry.
 * Owns no loops — enter/exit plus visibility is explicit via [activate]
 * / [deactivate] plus [requestExit] / [confirmExit] / [dismissExit].
 */
class DrivingViewModel(
    private val drive: DriveController,
    connectionManager: ConnectionManager,
    telemetryHub: TelemetryHub,
    sessionManager: SessionManager,
) : ViewModel() {

    private val _state = MutableStateFlow(DrivingUiState())
    val state: StateFlow<DrivingUiState> = _state.asStateFlow()

    private var manualEntered = false

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
        viewModelScope.launch {
            sessionManager.invalid.collect { invalid ->
                _state.update { it.copy(error = if (invalid) SessionManager.REPAIR_COPY else null) }
            }
        }
    }

    /**
     * Screen visible: enter manual then stream sticks at full rate.
     * Idempotent while entered — repeat activations (foreground return,
     * recomposition) are no-ops. Every [deactivate] re-arms, so the enter
     * follows visibility, not navigation.
     */
    fun activate() {
        if (manualEntered) return
        manualEntered = true
        drive.setInput { _state.value.throttle to _state.value.steering }
        drive.enterManual()
    }

    /**
     * Screen hidden: exit manual via [DriveController].
     * Idempotent — safe to race navigation-away against backgrounding.
     * Silent by design: no exit confirmation, the shell stays put.
     */
    fun deactivate() {
        if (!manualEntered) return
        manualEntered = false
        drive.exitManual()
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

    /**
     * Latches the emergency stop: zeroes the stick, one immediate stop
     * frame, then full-rate flagged stream until [onReleaseEmergency].
     * No-op unless Connected — with no link the robot's own stale-command
     * failsafe is already stopping it.
     */
    fun onEmergencyStop() {
        if (_state.value.aliveness != Aliveness.Connected) return
        _state.update { it.copy(eStopped = true, throttle = 0f, steering = 0f) }
        drive.emergencyStop()
    }

    /**
     * Releases a latched stop. Gated like [onEmergencyStop] so a latch held
     * across a link loss stays latched until telemetry re-enables.
     */
    fun onReleaseEmergency() {
        if (_state.value.aliveness != Aliveness.Connected) return
        _state.update { it.copy(eStopped = false) }
        drive.releaseEmergency()
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
