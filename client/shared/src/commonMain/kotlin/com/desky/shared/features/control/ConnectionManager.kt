package com.desky.shared.features.control

import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.Outcome
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.Telemetry
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.control.AliveUseCase
import com.desky.shared.features.control.ObserveRemoteDisconnectUseCase
import com.desky.shared.features.control.ObserveTelemetryUseCase
import com.desky.shared.features.control.PairRefreshUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

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
 * subscription-driven. Screens only collect [aliveness]; unpair tears the
 * session down (counters reset, PSK cache dropped, [aliveness] back to
 * [Aliveness.Unpaired]) while the loops idle for the next pairing. [stop]
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
 * dispatcher): tick counters are plain vars, the session cache serializes
 * under [sessionMutex] with lock order session-then-holder.
 */
class ConnectionManager(
    private val deviceRepo: DeviceRepository,
    private val getPairPsk: GetPairPskUseCase,
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

    private val sessionMutex = Mutex()
    private val currentDevice = MutableStateFlow<PairedDevice?>(null)
    private var sessionPsk: ByteArray? = null
    private var pskMissingFor: String? = null
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

    private var jobs: List<Job>? = null
    private var sessionScope: CoroutineScope? = null

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
        jobs = listOf(
            session.launch { collectPaired() },
            session.launch { collectTelemetry() },
            session.launch { collectRemoteDisconnect() },
            session.launch { runTickLoop() },
        )
    }

    /** Cancels all loops including any in-flight refresh; [start] relaunches. */
    fun stop() {
        sessionScope?.cancel()
        sessionScope = null
        jobs = null
    }

    private suspend fun collectPaired() {
        deviceRepo.observePaired().collect { paired ->
            val prev = currentDevice.value
            currentDevice.value = paired
            if (paired == null) {
                sessionMutex.withLock {
                    sessionPsk = null
                    pskMissingFor = null
                    toRobotCounter.reset()
                }
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
            if (dev == null) {
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
        val prepared = sessionMutex.withLock { sessionCounterLocked(dev) } ?: return
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
                    sessionMutex.withLock { refreshSessionPskLocked() }
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

    private suspend fun sessionCounterLocked(dev: PairedDevice): Pair<ByteArray, UInt>? {
        val psk = loadSessionPskLocked(dev) ?: return null
        toRobotCounter.ensure(dev.id, dev.lastCounter, dev.persistEvery, dev.era)
        return psk to (toRobotCounter.next() ?: return null)
    }

    private suspend fun loadSessionPskLocked(dev: PairedDevice): ByteArray? {
        var psk = sessionPsk
        if (psk == null) {
            if (pskMissingFor == dev.id) return null
            val hex = getPairPsk()
            if (hex == null) {
                pskMissingFor = dev.id
                return null
            }
            val decoded = try {
                hexToBytes(hex)
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                null
            }
            if (decoded == null || decoded.size != PAIR_PSK_BYTE_LENGTH) {
                pskMissingFor = dev.id
                return null
            }
            psk = decoded
            sessionPsk = psk
        }
        return psk
    }

    private suspend fun refreshSessionPskLocked() {
        sessionPsk = null
        pskMissingFor = null
        val dev = currentDevice.value ?: return
        loadSessionPskLocked(dev)
    }

    private companion object {
        const val PAIR_PSK_BYTE_LENGTH = 16
        const val REFRESH_RETRY_BACKOFF_MS = 5000L
        // Mirror the control ViewModel cadence: ALIVE every 500 ms sits far
        // inside the robot's 5 s alive window; silence past 2000 ms is dead.
        const val DEFAULT_ALIVE_INTERVAL_MS = 500L
        const val DEFAULT_DEAD_TIMEOUT_MS = 2000L
    }
}
