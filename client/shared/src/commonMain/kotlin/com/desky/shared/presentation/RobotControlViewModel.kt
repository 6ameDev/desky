package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.ESTOP_FLAG
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.GetBondPskUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.UdpSocketManager
import com.desky.shared.domain.hexToBytes
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

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

/** Room counter persist cadence: every N successful sends (mirrors firmware A1). */
private const val COUNTER_PERSIST_EVERY = 128

/**
 * Drives the robot-control screen (pairing/auth Task 9: every frame is an
 * authenticated `DRIVE_V2` datagram).
 *
 * On init the VM waits for the first non-null pairing, then starts two
 * child coroutines:
 * - telemetry collection, which promotes [RobotControlUiState.connectivity]
 *   to [Connectivity.Connected] and mirrors the packet mode/telemetry;
 * - the command send loop, which transmits the latest stick position every
 *   [tickMs] and NEVER pauses while paired — the centered heartbeat defeats
 *   the robot's ~500 ms stale-command failsafe
 *   ([STALE_MS][com.desky.shared.domain.STALE_MS]). While
 *   [RobotControlUiState.eStopped] is latched the loop sends centered
 *   sticks with [ESTOP_FLAG].
 *
 * Drive session: after the first pairing the loop loads the bond PSK via
 * [GetBondPskUseCase] ONCE and caches it in memory with the counter resumed
 * from the Room row (`lastCounter`), so the first wire packet uses counter
 * 1 after a fresh pair. Each send consumes the next counter; every 128th
 * successful send persists it to Room. A missing PSK surfaces
 * [Connectivity.Unpaired] + "Please re-pair to drive" and sends NOTHING
 * (never legacy frames); the miss is latched per device so the vault is
 * still read at most once (the idle loop does not retry).
 *
 * A later unpair pauses the send loop until re-pair (the loop re-reads
 * [currentDevice] every tick) and drops the cached PSK/counter, so the next
 * pairing reloads both fresh (one more vault read for the new session).
 * The session mutex serializes the loop tick against the immediate e-stop
 * frame so both share one strictly increasing counter sequence.
 * [tickMs] is a constructor parameter so tests can drive virtual time.
 *
 * @param udp transport handle, kept for API symmetry with the use cases
 *   (future direct-socket needs, e.g. lifecycle close); the VM itself only
 *   talks to the robot through the use cases.
 */
class RobotControlViewModel(
    private val sendUC: SendSignedControlUseCase,
    private val observeTelemetryUC: ObserveTelemetryUseCase,
    private val emergencyUC: EmergencyStopUseCase,
    private val deviceRepo: DeviceRepository,
    private val getBondPskUC: GetBondPskUseCase,
    private val udp: UdpSocketManager,
    private val tickMs: Long = 20,
) : ViewModel() {

    private val _state = MutableStateFlow(RobotControlUiState())
    val state: StateFlow<RobotControlUiState> = _state.asStateFlow()

    /** Last known pairing, tracked live: forgetting the device mid-session pauses sends. */
    private val currentDevice = MutableStateFlow<PairedDevice?>(null)

    /** Drive-session cache, guarded by [sessionMutex]. Null PSK = no session loaded. */
    private val sessionMutex = Mutex()
    private var sessionPsk: ByteArray? = null
    private var lastSentCounter: UInt = 0u
    private var sendsSincePersist: Int = 0
    /**
     * Device id whose vault lookup already came back empty. The loop idles
     * for it (no per-tick vault reads — the vault is read at most once per
     * session, missing or not); cleared on unpair so a re-pair reloads.
     */
    private var pskMissingFor: String? = null

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
                    if (dev == null) {
                        // Paused AND cache dropped: re-pair reloads PSK +
                        // counter fresh (a new vault read for the new session).
                        sessionMutex.withLock {
                            sessionPsk = null
                            pskMissingFor = null
                        }
                        delay(tickMs)
                        continue
                    }
                    val snapshot = _state.value
                    val cmd = if (snapshot.eStopped) {
                        ControlCommand(snapshot.mode, 0f, 0f, ESTOP_FLAG)
                    } else {
                        ControlCommand(snapshot.mode, snapshot.throttle, snapshot.steering, 0)
                    }
                    when (val result = sessionMutex.withLock { sendFrameLocked(dev, cmd) }) {
                        is Outcome.Success -> Unit
                        is Outcome.Failure ->
                            _state.update { it.copy(error = result.reason) }
                    }
                    delay(tickMs)
                }
            }
        }
    }

    /**
     * Sends one signed frame on the session, loading the session first when
     * empty. Must hold [sessionMutex]. On success the counter advances and
     * every 128th send persists to Room; on a missing PSK the failure tells
     * the user to re-pair (connectivity is forced back to Unpaired so the
     * screen never claims a live link it cannot drive).
     */
    private suspend fun sendFrameLocked(dev: PairedDevice, cmd: ControlCommand): Outcome<Unit> {
        val (psk, next) = sessionCounterLocked(dev) ?: return missingPsk()
        return when (val result = sendUC(dev, cmd, psk, next)) {
            is Outcome.Success -> {
                accountSendLocked(dev, next)
                result
            }
            is Outcome.Failure -> result
        }
    }

    /**
     * Session load + counter allocation shared by the loop tick and the
     * immediate e-stop frame, so both consume one strictly increasing
     * sequence. Must hold [sessionMutex]. Null = no PSK (caller reports
     * [missingPsk]); the `0u` exhaustion guard refuses to wrap (0 is never
     * valid on the wire — practically unreachable: 2^32 sends).
     */
    private suspend fun sessionCounterLocked(dev: PairedDevice): Pair<ByteArray, UInt>? {
        var psk = sessionPsk
        if (psk == null) {
            // Missing-PSK latch: the loop idles for this device instead of
            // reading the vault every tick (at most one vault read per
            // session, keyless or not).
            if (pskMissingFor == dev.id) return null
            val hex = getBondPskUC()
            if (hex == null) {
                pskMissingFor = dev.id
                return null
            }
            psk = hexToBytes(hex)
            sessionPsk = psk
            lastSentCounter = dev.lastCounter
            sendsSincePersist = 0
        }
        val next = lastSentCounter + 1u
        if (next == 0u) return null
        return psk to next
    }

    /** Records a successful send: advances the counter, persists every 128th. */
    private suspend fun accountSendLocked(dev: PairedDevice, sent: UInt) {
        lastSentCounter = sent
        sendsSincePersist++
        if (sendsSincePersist % COUNTER_PERSIST_EVERY == 0) {
            deviceRepo.updateCounter(dev.id, sent)
        }
    }

    private fun missingPsk(): Outcome.Failure {
        _state.update {
            it.copy(
                connectivity = Connectivity.Unpaired,
                error = "Please re-pair to drive",
            )
        }
        return Outcome.Failure("Please re-pair to drive")
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
     * Latches the e-stop: sticks center, a signed estop frame goes out
     * immediately via [emergencyUC] (sharing the session counter with the
     * loop tick under [sessionMutex]), and the send loop keeps transmitting
     * centered+[ESTOP_FLAG] until [onReleaseEmergency]. When no session is
     * loaded yet the immediate frame is skipped — the loop sends the latched
     * variant on its next tick.
     */
    fun onEmergencyStop() {
        _state.update { it.copy(eStopped = true, throttle = 0f, steering = 0f) }
        viewModelScope.launch {
            val paired = currentDevice.value ?: return@launch
            val result = sessionMutex.withLock {
                val (psk, next) = sessionCounterLocked(paired) ?: return@withLock null
                when (val r = emergencyUC(paired, psk, next)) {
                    is Outcome.Success -> {
                        accountSendLocked(paired, next)
                        r
                    }
                    is Outcome.Failure -> r
                }
            } ?: return@launch
            if (result is Outcome.Failure) {
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
