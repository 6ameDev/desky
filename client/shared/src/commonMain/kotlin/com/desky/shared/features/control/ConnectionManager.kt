package com.desky.shared.features.control

import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.Outcome
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.Telemetry
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.features.session.SessionManager
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay

/**
 * Link aliveness from the app's side of a pairing.
 */
enum class Aliveness {
    Unpaired,
    Connected,
    Stale,
    Dead,
}

/**
 * Always-on-while-paired connection owner: sends `ALIVE` on cadence while a
 * device is paired, watches inbound telemetry with a silence watchdog,
 * observes verified core-originated remote disconnects, and triggers the
 * silent pair-refresh rotation when due.
 *
 * Runs while paired regardless of subscribers: all loops are children of
 * [scope] started on creation (or explicit [start]), never
 * subscription-driven. Screens only collect [aliveness]; unpair returns
 * [aliveness] to [Aliveness.Unpaired] while [SessionManager] drops the key
 * and resets the counter, and the loops idle for the next pairing. [stop]
 * cancels everything for owner teardown.
 *
 * Cadence/timeout production defaults mirror the control ViewModel:
 * ALIVE every 500 ms (far inside the robot's 5 s alive window), dead after
 * 2000 ms of silence (one packet must not latch Connected forever), stale
 * at half the dead timeout. Ticks count send-loop iterations, never wall-clock, so tests
 * drive virtual time.
 *
 * State machine: fresh pairing lands on [Aliveness.Stale] (paired, link
 * unproven); any telemetry promotes to [Aliveness.Connected]; silence past
 * [staleTimeoutMs] demotes to [Aliveness.Stale], past [deadTimeoutMs] to
 * [Aliveness.Dead]. `ALIVE` keeps transmitting in every paired state —
 * telemetry is the ACK, and send failures never surface.
 *
 * Assumes [scope] confines the loops to one thread (Main or a test
 * dispatcher): tick counters are plain vars, session allocation
 * serializes inside [SessionManager].
 */
class ConnectionManager(
    private val deviceRepo: DeviceRepository,
    private val sessionManager: SessionManager,
    private val observeTelemetry: ObserveTelemetryUseCase,
    private val toRobotCounter: ToRobotCounter,
    private val aliveUC: AliveUseCase,
    private val remoteDisconnectUC: ObserveRemoteDisconnectUseCase,
    private val scope: CoroutineScope,
    private val pairRefreshUC: PairRefreshUseCase? = null,
    private val aliveIntervalMs: Long = DEFAULT_ALIVE_INTERVAL_MS,
    private val staleTimeoutMs: Long = DEFAULT_DEAD_TIMEOUT_MS / 2,
    private val deadTimeoutMs: Long = DEFAULT_DEAD_TIMEOUT_MS,
    private val tickMs: Long = 20,
    private val isRefreshDue: suspend () -> Boolean = { toRobotCounter.needsPairRefresh() },
) {
    private val _aliveness = MutableStateFlow(Aliveness.Unpaired)

    /** Latest link state; replays to late subscribers. Never Unpaired while paired. */
    val aliveness: StateFlow<Aliveness> = _aliveness.asStateFlow()

    private val currentDevice = MutableStateFlow<PairedDevice?>(null)
    private var pairRefreshInFlight = false
    private var refreshBackoffTicks = 0
    private var ticksSinceAlive = 0
    private var ticksSinceTelemetry = 0

    private val aliveTickEvery = maxOf(1, (aliveIntervalMs / tickMs.coerceAtLeast(1)).toInt())
    private val staleTimeoutTicks = maxOf(1, (staleTimeoutMs / tickMs.coerceAtLeast(1)).toInt())
    private val deadTimeoutTicks =
        maxOf(staleTimeoutTicks, (deadTimeoutMs / tickMs.coerceAtLeast(1)).toInt())
    private val refreshBackoffTickCount =
        maxOf(1, (REFRESH_RETRY_BACKOFF_MS / tickMs.coerceAtLeast(1)).toInt())

    private var sessionScope: CoroutineScope? = null
    private var backgrounded = false

    init {
        start()
    }

    /** Idempotent: launches the paired-gated loops as children of [scope]. */
    fun start() {
        if (sessionScope != null) return
        // Child scope supervised apart from the owner: stopping cancels every
        // loop (including in-flight refresh) without touching the owner.
        val session = CoroutineScope(scope.coroutineContext + SupervisorJob(scope.coroutineContext[Job]))
        sessionScope = session
        ticksSinceAlive = aliveTickEvery
        session.launch { collectPaired() }
        session.launch { collectTelemetry() }
        session.launch { collectRemoteDisconnect() }
        session.launch { runTickLoop() }
    }

    /** Cancels all loops including any in-flight refresh; [start] relaunches. */
    fun stop() {
        sessionScope?.cancel()
        sessionScope = null
    }

    /**
     * App backgrounded: goodbye without unpairing. One `DISCONNECT` frame
     * allocated once and sent twice byte-identical, then ALIVE and refresh
     * pause until [onForeground] while the loops keep collecting. No pair
     * means no-op. Fed by one platform lifecycle observer — never by screens.
     */
    fun onBackground() {
        backgrounded = true
        val dev = currentDevice.value ?: return
        val session = sessionScope ?: return
        session.launch {
            val prepared = sessionManager.allocate(dev) ?: return@launch
            val (psk, counter) = prepared
            aliveUC.sendDisconnect(dev, psk, dev.era, counter)
            aliveUC.sendDisconnect(dev, psk, dev.era, counter)
        }
    }

    /**
     * App foregrounded: ALIVE goes due immediately instead of waiting out
     * the cadence. Watchdog time frozen during background is simply skipped —
     * ticks never advanced while [backgrounded], so there is no catch-up
     * demotion. No pair means no-op beyond clearing the flag.
     */
    fun onForeground() {
        backgrounded = false
        ticksSinceAlive = aliveTickEvery
    }

    private suspend fun collectPaired() {
        deviceRepo.observePaired().collect { paired ->
            val prev = currentDevice.value
            currentDevice.value = paired
            if (paired == null) {
                pairRefreshInFlight = false
                refreshBackoffTicks = 0
                ticksSinceTelemetry = 0
                ticksSinceAlive = aliveTickEvery
                _aliveness.value = Aliveness.Unpaired
            } else if (prev?.id != paired.id || prev.era != paired.era) {
                // Fresh pairing or era rotation: re-arm the cadence. Counter-only
                // Room persists (same id + era) fall through without touching
                // ticks, so steady sending never masks watchdog silence.
                ticksSinceAlive = aliveTickEvery
                ticksSinceTelemetry = staleTimeoutTicks
                if (_aliveness.value == Aliveness.Unpaired) {
                    _aliveness.value = Aliveness.Stale
                }
            }
        }
    }

    private suspend fun collectTelemetry() {
        observeTelemetry().collect {
            if (currentDevice.value != null) {
                ticksSinceTelemetry = 0
                if (_aliveness.value != Aliveness.Connected) {
                    _aliveness.value = Aliveness.Connected
                }
            }
        }
    }

    private suspend fun collectRemoteDisconnect() {
        // Hot eviction stream: verify + local forget already ran per emission
        // inside the use case. Collecting here drives verification; the
        // resulting repo null flows through collectPaired into Unpaired.
        remoteDisconnectUC.observe().collect { }
    }

    private suspend fun runTickLoop() {
        while (true) {
            val dev = currentDevice.value
            if (dev == null || backgrounded) {
                delay(tickMs)
                continue
            }
            noteSilenceTick()
            sendAliveIfDue(dev)
            checkRefresh()
            delay(tickMs)
        }
    }

    /** Watchdog: silence past the stale/dead marks demotes the link (monotonic). */
    private fun noteSilenceTick() {
        if (ticksSinceTelemetry < deadTimeoutTicks) ticksSinceTelemetry++
        val degraded = when {
            ticksSinceTelemetry >= deadTimeoutTicks -> Aliveness.Dead
            ticksSinceTelemetry >= staleTimeoutTicks -> Aliveness.Stale
            else -> null
        }
        val current = _aliveness.value
        if (degraded != null && current != Aliveness.Unpaired && degraded > current) {
            _aliveness.value = degraded
        }
    }

    /** Sends one `ALIVE` when the cadence is due; skipped ticks cost nothing. */
    private suspend fun sendAliveIfDue(dev: PairedDevice) {
        if (ticksSinceAlive < aliveTickEvery) ticksSinceAlive++
        if (ticksSinceAlive < aliveTickEvery) return
        ticksSinceAlive = 0
        val prepared = sessionManager.allocate(dev) ?: return
        val (psk, counter) = prepared
        aliveUC(dev, psk, dev.era, counter)
    }

    /** Fires the silent pair-refresh rotation when due (backed off on failure). */
    private suspend fun checkRefresh() {
        if (refreshBackoffTicks > 0) {
            refreshBackoffTicks--
            return
        }
        if (pairRefreshUC != null && !pairRefreshInFlight && isRefreshDue()) {
            triggerPairRefresh()
        }
    }

    private fun triggerPairRefresh() {
        val session = sessionScope ?: return
        pairRefreshInFlight = true
        session.launch {
            try {
                val result = pairRefreshUC?.invoke()
                if (result is Outcome.Success) {
                    sessionManager.invalidate()
                } else {
                    refreshBackoffTicks = refreshBackoffTickCount
                }
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                refreshBackoffTicks = refreshBackoffTickCount
            } finally {
                pairRefreshInFlight = false
            }
        }
    }

    private companion object {
        const val REFRESH_RETRY_BACKOFF_MS = 5000L
        // Mirror the control ViewModel cadence: ALIVE every 500 ms sits far
        // inside the robot's 5 s alive window; silence past 2000 ms is dead.
        const val DEFAULT_ALIVE_INTERVAL_MS = 500L
        const val DEFAULT_DEAD_TIMEOUT_MS = 2000L
    }
}
