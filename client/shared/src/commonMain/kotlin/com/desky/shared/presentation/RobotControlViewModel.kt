package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.ESTOP_FLAG
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendRobotControlCommandUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.UdpSocketManager
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/** Link state of the control screen. */
enum class Connectivity {
    Disconnected,
    Unpaired,
    Connected,
}

/** Drive-screen state (UDF: single [StateFlow] + explicit intents below). */
data class RobotControlUiState(
    val throttle: Float = 0f,
    val steering: Float = 0f,
    val telemetry: Telemetry? = null,
    val connectivity: Connectivity = Connectivity.Disconnected,
    val mode: RobotMode = RobotMode.MANUAL,
    val eStopped: Boolean = false,
    val error: String? = null,
)

/**
 * Drives the robot-control screen.
 *
 * On init the VM waits for the first non-null pairing, then starts two
 * child coroutines:
 * - telemetry collection, which promotes [RobotControlUiState.connectivity]
 *   to [Connectivity.Connected] and mirrors the packet mode/telemetry;
 * - the command send loop, which transmits the latest stick position every
 *   [tickMs] and NEVER pauses — the centered heartbeat defeats the robot's
 *   ~500 ms stale-command failsafe ([STALE_MS][com.desky.shared.domain.STALE_MS]).
 *   While [RobotControlUiState.eStopped] is latched the loop sends centered
 *   sticks with [ESTOP_FLAG].
 *
 * A missing pairing surfaces as [Connectivity.Unpaired] until telemetry
 * arrives; a later unpair pauses the send loop until re-pair.
 * [tickMs] is a constructor parameter so tests can drive virtual
 * time.
 *
 * @param udp transport handle, kept for API symmetry with the use cases
 *   (future direct-socket needs, e.g. lifecycle close); the VM itself only
 *   talks to the robot through the use cases.
 */
class RobotControlViewModel(
    private val sendUC: SendRobotControlCommandUseCase,
    private val observeTelemetryUC: ObserveTelemetryUseCase,
    private val emergencyUC: EmergencyStopUseCase,
    private val deviceRepo: DeviceRepository,
    private val udp: UdpSocketManager,
    private val tickMs: Long = 20,
) : ViewModel() {

    private val _state = MutableStateFlow(RobotControlUiState())
    val state: StateFlow<RobotControlUiState> = _state.asStateFlow()

    /** Last known pairing, tracked live: forgetting the device mid-session pauses sends. */
    private val currentDevice = MutableStateFlow<PairedDevice?>(null)

    init {
        viewModelScope.launch {
            deviceRepo.observePaired().collect { paired ->
                currentDevice.value = paired
                if (paired == null) {
                    _state.update { it.copy(connectivity = Connectivity.Unpaired) }
                }
            }
        }
        viewModelScope.launch {
            // Gate: loops start on the first pairing. The send loop below
            // re-reads currentDevice every tick, so a later unpair pauses
            // sending (heartbeat resumes on re-pair) instead of driving a
            // forgotten robot on a stale snapshot.
            deviceRepo.observePaired().filterNotNull().first()
            launch {
                observeTelemetryUC().collect { telemetry ->
                    _state.update {
                        it.copy(
                            telemetry = telemetry,
                            mode = telemetry.mode,
                            connectivity = Connectivity.Connected,
                        )
                    }
                }
            }
            launch {
                while (true) {
                    val dev = currentDevice.value
                    if (dev != null) {
                        val snapshot = _state.value
                        val cmd = if (snapshot.eStopped) {
                            ControlCommand(snapshot.mode, 0f, 0f, ESTOP_FLAG)
                        } else {
                            ControlCommand(snapshot.mode, snapshot.throttle, snapshot.steering, 0)
                        }
                        when (val result = sendUC(dev, cmd)) {
                            is Outcome.Success -> Unit
                            is Outcome.Failure ->
                                _state.update { it.copy(error = result.reason) }
                        }
                    }
                    delay(tickMs)
                }
            }
        }
    }

    /** Stick deflection in -1..1 (clamped). Never clears a latched e-stop. */
    fun onStick(throttle: Float, steering: Float) {
        _state.update {
            it.copy(
                throttle = throttle.coerceIn(-1f, 1f),
                steering = steering.coerceIn(-1f, 1f),
            )
        }
    }

    /**
     * Latches the e-stop: sticks center, an estop frame goes out immediately
     * via [emergencyUC], and the send loop keeps transmitting
     * centered+[ESTOP_FLAG] until [onReleaseEmergency].
     */
    fun onEmergencyStop() {
        _state.update { it.copy(eStopped = true, throttle = 0f, steering = 0f) }
        viewModelScope.launch {
            val paired = currentDevice.value ?: return@launch
            when (val result = emergencyUC(paired)) {
                is Outcome.Success -> Unit
                is Outcome.Failure ->
                    _state.update { it.copy(error = result.reason) }
            }
        }
    }

    fun onReleaseEmergency() {
        _state.update { it.copy(eStopped = false) }
    }

    // No explicit cancel: the telemetry + send loops are children of
    // viewModelScope, so they are cancelled automatically here. The robot's
    // own ~500 ms stale-command failsafe covers the stop.
    override fun onCleared() {
        super.onCleared()
    }
}
