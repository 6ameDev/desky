package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.DriveCounter
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

/**
 * Telemetry link timeout: silence longer than this demotes
 * [RobotControlUiState.connectivity] from [Connectivity.Connected] back to
 * [Connectivity.Disconnected] (M2 — one packet used to latch Connected
 * forever). Counted in send-loop ticks inside the ViewModel (timeout / tick),
 * never wall-clock, so tests drive it with virtual time.
 */
const val TELEMETRY_TIMEOUT_MS = 2000L

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
 * Drives the robot-control screen (pairing/auth Task 9: every frame is an
 * authenticated `DRIVE_V2` datagram).
 *
 * On init the VM waits for the first non-null pairing, then starts two
 * child coroutines:
 * - telemetry collection, which promotes [RobotControlUiState.connectivity]
 *   to [Connectivity.Connected] and mirrors the packet mode/telemetry;
 *   silence longer than [TELEMETRY_TIMEOUT_MS] demotes back to
 *   [Connectivity.Disconnected] (the last packet stays visible);
 * - the command send loop, which transmits the latest stick position every
 *   [tickMs] and NEVER pauses while paired — the centered heartbeat defeats
 *   the robot's ~500 ms stale-command failsafe
 *   ([STALE_MS][com.desky.shared.domain.STALE_MS]). While
 *   [RobotControlUiState.eStopped] is latched the loop sends centered
 *   sticks with [ESTOP_FLAG].
 *
 * Drive session: after the first pairing the loop loads the bond PSK via
 * [GetBondPskUseCase] ONCE and caches it in memory, seeding the shared
 * [DriveCounter] from the Room row (`lastCounter`), so the first wire
 * packet uses counter 1 after a fresh pair. Each send allocates the next
 * counter from the holder (which persists to Room every 128th allocation).
 * A missing PSK surfaces [Connectivity.Unpaired] + "Please re-pair to
 * drive" and sends NOTHING (never legacy frames); the miss is latched per
 * device so the vault is still read at most once (the idle loop does not
 * retry).
 *
 * A later unpair pauses the send loop until re-pair (the loop re-reads
 * [currentDevice] every tick) and drops the cached PSK plus the holder era,
 * so the next pairing reloads both fresh (one more vault read for the new
 * session). The session mutex serializes counter allocation between the
 * loop tick and the immediate e-stop frame so both share one strictly
 * increasing counter sequence; the actual network send always happens
 * OUTSIDE the lock so a stalled send never blocks the other path.
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
    private val driveCounter: DriveCounter,
    private val tickMs: Long = 20,
) : ViewModel() {

    private val _state = MutableStateFlow(RobotControlUiState())
    val state: StateFlow<RobotControlUiState> = _state.asStateFlow()

    /** Last known pairing, tracked live: forgetting the device mid-session pauses sends. */
    private val currentDevice = MutableStateFlow<PairedDevice?>(null)

    /**
     * Send-loop ticks since the last telemetry packet — the M2 timeout
     * basis. Reset by the telemetry collector, incremented once per paired
     * send-loop iteration (one tick == one [tickMs], so the timeout is
     * virtual-time friendly). Both coroutines run on `viewModelScope`
     * (Main), so a plain var suffices — no suspend point sits between any
     * read and the matching write.
     */
    private var ticksSinceTelemetry = 0

    /** Silent send-loop ticks before Connected demotes to Disconnected. */
    private val telemetryTimeoutTicks: Int =
        maxOf(1, (TELEMETRY_TIMEOUT_MS / tickMs.coerceAtLeast(1)).toInt())

    /** Drive-session cache, guarded by [sessionMutex]. Null PSK = no session loaded. */
    private val sessionMutex = Mutex()
    private var sessionPsk: ByteArray? = null
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
                    ticksSinceTelemetry = 0
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
                            driveCounter.reset()
                        }
                        ticksSinceTelemetry = 0
                        delay(tickMs)
                        continue
                    }
                    // M2 telemetry timeout: one packet must not latch
                    // Connected forever. Past the silence budget demote to
                    // Disconnected (last packet stays visible); only ever
                    // demotes FROM Connected, so the pre-telemetry Unpaired
                    // state is untouched.
                    if (ticksSinceTelemetry < telemetryTimeoutTicks) {
                        ticksSinceTelemetry++
                    }
                    if (ticksSinceTelemetry >= telemetryTimeoutTicks &&
                        _state.value.connectivity == Connectivity.Connected
                    ) {
                        _state.update { it.copy(connectivity = Connectivity.Disconnected) }
                    }
                    val snapshot = _state.value
                    val cmd = if (snapshot.eStopped) {
                        ControlCommand(snapshot.mode, 0f, 0f, ESTOP_FLAG)
                    } else {
                        ControlCommand(snapshot.mode, snapshot.throttle, snapshot.steering, 0)
                    }
                    // Allocate (psk, counter) under the session mutex, then
                    // send OUTSIDE the lock: udp.send has unbounded latency
                    // and must never stall the e-stop path behind a loop tick.
                    val prepared = sessionMutex.withLock { sessionCounterLocked(dev) }
                    val result = if (prepared == null) {
                        missingPsk()
                    } else {
                        val (psk, next) = prepared
                        sendUC(dev, cmd, psk, next)
                    }
                    when (result) {
                        is Outcome.Success ->
                            if (_state.value.error != null) {
                                _state.update { it.copy(error = null) }
                            }
                        is Outcome.Failure ->
                            if (_state.value.error != result.reason) {
                                _state.update { it.copy(error = result.reason) }
                            }
                    }
                    delay(tickMs)
                }
            }
        }
    }

    /**
     * Session load + counter allocation shared by the loop tick and the
     * immediate e-stop frame, so both consume one strictly increasing
     * sequence from the shared [DriveCounter]. Must hold [sessionMutex].
     * Returns null when there is no PSK or no counter available (caller
     * reports [missingPsk] for the loop tick, or skips the immediate frame
     * for e-stop — the loop sends the latched variant on its next tick).
     * Never performs network I/O: callers send OUTSIDE the lock so a
     * stalled send cannot block the other path's allocation.
     * The holder's `0u` exhaustion guard refuses to wrap (0 is never valid
     * on the wire — practically unreachable: 2^32 sends).
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
        }
        // Seed from Room on a fresh era only; a live era is a no-op, so the
        // watermark can never rewind the sequence mid-session.
        driveCounter.ensure(dev.id, dev.lastCounter)
        val next = driveCounter.next() ?: return null
        return psk to next
    }

    private fun missingPsk(): Outcome.Failure {
        val message = "Please re-pair to drive"
        val current = _state.value
        if (current.connectivity != Connectivity.Unpaired || current.error != message) {
            _state.update {
                it.copy(
                    connectivity = Connectivity.Unpaired,
                    error = message,
                )
            }
        }
        return Outcome.Failure(message)
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
            // Allocate under the session mutex, send outside it: an
            // in-flight loop send never blocks this immediate frame's
            // allocation (and vice versa).
            val prepared = sessionMutex.withLock { sessionCounterLocked(paired) }
                ?: return@launch
            val (psk, next) = prepared
            val result = emergencyUC(paired, psk, next)
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
