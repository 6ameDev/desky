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
import kotlin.math.abs

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

/**
 * Idle heartbeat period: while the sticks sit centered (and the e-stop is
 * clear) the send loop transmits only this often instead of every [tickMs].
 * 200 ms is far below the robot's ~500 ms stale-command failsafe
 * ([STALE_MS][com.desky.shared.domain.STALE_MS]) — a 2.5x margin — so the
 * failsafe can never trip, while radio/CPU/battery cost drops ~10x at rest.
 * Any stick deflection, latched e-stop, or stick activity within the last
 * [IDLE_HEARTBEAT_MS] restores full-rate ([tickMs]) sends immediately.
 */
const val IDLE_HEARTBEAT_MS = 200L

/**
 * Telemetry UI coalescing window: the robot streams telemetry at ~15 Hz
 * ([CFG_TELEMETRY_HZ][com.desky.shared.domain.CFG_TELEMETRY_HZ]), but the
 * screen only needs ~5 Hz. Every packet still resets the link timeout and
 * promotes [RobotControlUiState.connectivity] to [Connectivity.Connected]
 * (cheap logic path), while the heavy `telemetry`/`mode` copy into [_state]
 * — which recomposes the whole screen — happens at most once per
 * [TELEMETRY_UI_MS], or immediately on a high-value edge (mode flip, cliff
 * bit flip, driving bit flip). Counted in send-loop ticks (virtual-time
 * friendly, like the idle heartbeat); same 200 ms value as
 * [IDLE_HEARTBEAT_MS] by design, but a separate constant so the two rates
 * can diverge.
 */
const val TELEMETRY_UI_MS = 200L

/**
 * Stick deflection above this magnitude counts as active driving (full
 * 50 Hz). Below it the sticks read as centered (stick noise/rest never holds
 * the radio at full rate).
 */
const val STICK_ACTIVE_THRESHOLD = 0.02f

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
 *   to [Connectivity.Connected] on EVERY packet (cheap logic path: timeout
 *   reset + Connected promotion stay snappy at the full 15 Hz) but copies
 *   the packet mode/telemetry into state at most every [TELEMETRY_UI_MS]
 *   (~5 Hz) or on a high-value edge (mode flip, cliff bit flip, driving
 *   bit flip), so the screen recomposes at ~5 Hz instead of 15 Hz;
 *   silence longer than [TELEMETRY_TIMEOUT_MS] demotes back to
 *   [Connectivity.Disconnected] (the last packet stays visible);
 * - the command send loop, which transmits the latest stick position at
 *   full rate (every [tickMs], 50 Hz) while driving and idles down to one
 *   heartbeat every [IDLE_HEARTBEAT_MS] (~5 Hz) on centered sticks — either
 *   way the heartbeat defeats the robot's ~500 ms stale-command failsafe
 *   ([STALE_MS][com.desky.shared.domain.STALE_MS]). While
 *   [RobotControlUiState.eStopped] is latched the loop sends centered
 *   sticks with [ESTOP_FLAG] at full rate.
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
 * Lifecycle / ownership (no behavior change while driving):
 * - the telemetry + send loops are children of `viewModelScope` and stop
 *   automatically in [onCleared] (the robot's ~500 ms stale-command failsafe
 *   covers the stop);
 * - [onBackground]/[onForeground] pause/resume the send loop when the
 *   control screen leaves/returns (flag checked each tick: skips send +
 *   counter alloc, still delays). Unpair pauses the same way.
 *
 * @param udp transport handle, kept for API symmetry with the use cases;
 *   the VM itself only talks to the robot through the use cases and MUST
 *   NOT close it. Ownership: the socket is a Koin app-scope singleton
 *   (see `dataModule`), closed only by the app scope (`DeskyApp`), never
 *   by a ViewModel — closing it here would break every other screen.
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
     * Background pause flag for [onBackground]/[onForeground]. Checked once
     * per send-loop tick: while true the tick skips vault/counter/send and
     * just delays (no counter allocated, session cache kept so resume needs
     * no vault re-read). Volatile so a background call from any thread is
     * visible to the Main loop without a lock.
     */
    @Volatile
    private var backgroundPaused = false

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

    /**
     * Idle-heartbeat gating, counted in send-loop ticks (virtual-time
     * friendly, like [ticksSinceTelemetry]). [ticksSinceActivity] resets on
     * every actively-driven tick (stick deflection, latched e-stop, or a
     * deflected [onStick] between ticks) and caps at [idleTickEvery], so the
     * 200 ms after any activity stays full-rate; [ticksSinceIdleSend] spaces
     * the ~5 Hz heartbeats once idle. Both run on `viewModelScope` (Main),
     * so plain vars suffice — see [ticksSinceTelemetry].
     */
    private var ticksSinceActivity = 0
    private var ticksSinceIdleSend = 0

    /** Paired-loop ticks per idle heartbeat ([IDLE_HEARTBEAT_MS] / [tickMs]). */
    private val idleTickEvery: Int =
        maxOf(1, (IDLE_HEARTBEAT_MS / tickMs.coerceAtLeast(1)).toInt())

    /** Paired-loop ticks per telemetry UI copy ([TELEMETRY_UI_MS] / [tickMs]). */
    private val telemetryUiEvery: Int =
        maxOf(1, (TELEMETRY_UI_MS / tickMs.coerceAtLeast(1)).toInt())

    /**
     * Send-loop ticks since the last telemetry UI copy — the ~5 Hz
     * coalescing clock. Reset by the telemetry collector on every UI copy,
     * incremented once per paired send-loop iteration (virtual-time
     * friendly). Starts due so the first packet paints immediately. Runs
     * on `viewModelScope` (Main), so a plain var suffices — see
     * [ticksSinceTelemetry].
     */
    private var ticksSinceTelemetryUi = 10_000

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
                    // Logic path (every packet, cheap): the link is alive.
                    ticksSinceTelemetry = 0
                    val prev = _state.value
                    // UI path (gated, expensive): copy telemetry/mode only
                    // when due or high-value, so a 15 Hz stream recomposes
                    // at ~5 Hz. Promotion from non-Connected always paints
                    // (fresh packet after silence must be visible); edges
                    // (mode/cliff/driving flips) paint immediately even
                    // inside the window. Otherwise skip [_state] entirely —
                    // already Connected means "ensure Connected" is a no-op.
                    val highValueChange = prev.telemetry == null ||
                        prev.mode != telemetry.mode ||
                        prev.telemetry.mode != telemetry.mode ||
                        prev.telemetry.cliff != telemetry.cliff ||
                        prev.telemetry.driving != telemetry.driving
                    val timeDue = ticksSinceTelemetryUi >= telemetryUiEvery
                    if (prev.connectivity != Connectivity.Connected || highValueChange || timeDue) {
                        ticksSinceTelemetryUi = 0
                        _state.update {
                            it.copy(
                                telemetry = telemetry,
                                mode = telemetry.mode,
                                connectivity = Connectivity.Connected,
                            )
                        }
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
                        ticksSinceActivity = 0
                        ticksSinceIdleSend = 0
                        ticksSinceTelemetryUi = telemetryUiEvery
                        delay(tickMs)
                        continue
                    }
                    // M2 telemetry timeout: one packet must not latch
                    // Connected forever. Past the silence budget demote to
                    // Disconnected (last packet stays visible); only ever
                    // demotes FROM Connected, so the pre-telemetry Unpaired
                    // state is untouched. The UI coalescing clock advances
                    // on the same wall ticks so the ~5 Hz gate runs on
                    // virtual time.
                    if (ticksSinceTelemetry < telemetryTimeoutTicks) {
                        ticksSinceTelemetry++
                    }
                    if (ticksSinceTelemetryUi < telemetryUiEvery) {
                        ticksSinceTelemetryUi++
                    }
                    if (ticksSinceTelemetry >= telemetryTimeoutTicks &&
                        _state.value.connectivity == Connectivity.Connected
                    ) {
                        _state.update { it.copy(connectivity = Connectivity.Disconnected) }
                    }
                    // Background pause: skip vault/counter/send, keep the
                    // tick delay. Session cache is kept (unlike unpair), so
                    // resume needs no vault re-read and counters stay gapless.
                    if (backgroundPaused) {
                        delay(tickMs)
                        continue
                    }
                    val snapshot = _state.value
                    // Idle heartbeat: centered + e-stop-clear + no activity
                    // in the last IDLE_HEARTBEAT_MS idles to ~5 Hz; anything
                    // else sends every tick. Telemetry counting above stays
                    // on wall ticks (it already ran), and skipped ticks
                    // allocate NO counter (DriveCounter.next only runs on a
                    // real send) and leave a latched error untouched.
                    val driveActive = snapshot.eStopped ||
                        abs(snapshot.throttle) > STICK_ACTIVE_THRESHOLD ||
                        abs(snapshot.steering) > STICK_ACTIVE_THRESHOLD
                    if (driveActive) {
                        ticksSinceActivity = 0
                    } else if (ticksSinceActivity < idleTickEvery) {
                        ticksSinceActivity++
                    }
                    val fullRate = driveActive || ticksSinceActivity < idleTickEvery
                    if (fullRate) {
                        ticksSinceIdleSend = 0
                    } else {
                        ticksSinceIdleSend++
                        if (ticksSinceIdleSend < idleTickEvery) {
                            delay(tickMs)
                            continue
                        }
                        ticksSinceIdleSend = 0
                    }
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

    /**
     * Stick deflection in -1..1 (clamped) with center deadband + distinct
     * check: values with abs below [STICK_ACTIVE_THRESHOLD] snap to 0f, and
     * a call whose snapped values equal the current state skips [_state]
     * entirely (touch events arrive at 60-120 Hz; the send loop samples
     * every [tickMs], so steady holds would otherwise recompose ~80-90%
     * needlessly). Never clears a latched e-stop. A deflected (snapped)
     * call marks stick activity immediately, so even a blip that recenters
     * before the next loop tick holds the loop at full rate for
     * [IDLE_HEARTBEAT_MS] (the loop re-derives activity from the latched
     * state every tick regardless).
     */
    fun onStick(throttle: Float, steering: Float) {
        val snappedThrottle = throttle.coerceIn(-1f, 1f).let {
            if (abs(it) < STICK_ACTIVE_THRESHOLD) 0f else it
        }
        val snappedSteering = steering.coerceIn(-1f, 1f).let {
            if (abs(it) < STICK_ACTIVE_THRESHOLD) 0f else it
        }
        if (abs(snappedThrottle) > STICK_ACTIVE_THRESHOLD ||
            abs(snappedSteering) > STICK_ACTIVE_THRESHOLD
        ) {
            ticksSinceActivity = 0
        }
        val current = _state.value
        // distinctUntilChanged: e-stop is never touched here, so throttle +
        // steering equality alone means no visible change — skip the update.
        if (current.throttle == snappedThrottle && current.steering == snappedSteering) return
        _state.update {
            it.copy(
                throttle = snappedThrottle,
                steering = snappedSteering,
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

    /**
     * Pause sending while the control screen is in the background (leaving
     * the room turns off the lights). The next send-loop tick skips
     * vault/counter/send and just delays; the session cache is kept, so
     * [onForeground] resumes with no vault re-read and gapless counters.
     * Telemetry collection and the timeout counter keep running.
     */
    fun onBackground() {
        backgroundPaused = true
    }

    /** Resume sending after [onBackground]. */
    fun onForeground() {
        backgroundPaused = false
    }

    /**
     * Loops are children of `viewModelScope`, so they are cancelled
     * automatically here (explicit cancel unnecessary — the robot's own
     * ~500 ms stale-command failsafe covers the stop).
     *
     * Best-effort session drop, synchronously only: clears the VM-local
     * [sessionPsk]/[pskMissingFor] when the session mutex is uncontended
     * ([Mutex.tryLock]), otherwise skips (memory is reclaimed with the VM
     * anyway — never suspend or leak a coroutine from `onCleared`).
     * Deliberately does NOT close [udp] (app-scope Koin singleton owned by
     * `DeskyApp` — closing it here would break every other screen) and does
     * NOT reset the shared [driveCounter] (its live era must survive the VM
     * so the next session keeps a strictly increasing sequence; resetting
     * to the stale Room watermark would look like a replay to the robot).
     */
    override fun onCleared() {
        if (sessionMutex.tryLock()) {
            try {
                sessionPsk = null
                pskMissingFor = null
            } finally {
                sessionMutex.unlock()
            }
        }
        super.onCleared()
    }
}
