package com.desky.shared.features.control

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.RobotMode
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.control.ReleaseManualUseCase
import com.desky.shared.features.control.SendSignedControlUseCase
import com.desky.shared.features.control.TakeManualUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlin.math.abs

/**
 * Session-scope drive owner: the manual take/release bind plus the drive
 * frame loop, standalone behind the existing API (the control ViewModel is
 * not rewired to it yet, so telemetry MANUAL gating, e-stop, and error
 * surfacing stay with the ViewModel until the rewire).
 *
 * Idle loop runs whenever paired, bound or not: unbound it sends zeroed
 * `DRIVE` frames on the idle heartbeat (far inside the robot's ~500 ms
 * stale-command failsafe, so the robot never starves); [bind] sends one
 * `MANUAL_TAKE` then streams the supplied input at full rate; [unbind]
 * sends best-effort `MANUAL_RELEASE` then drops back to idle zeroed
 * heartbeats. [stop] cancels everything for owner teardown.
 *
 * Cadence defaults mirror the control ViewModel ([tickMs] 20,
 * [idleHeartbeatMs] 200, [stickActiveThreshold] 0.02): ticks count loop
 * iterations, never wall-clock, so tests drive virtual time. Skipped idle
 * ticks allocate no counter.
 *
 * Assumes [scope] confines the loops to one thread (Main or a test
 * dispatcher): tick counters and the bound flag are plain vars, the session
 * cache serializes under [sessionMutex] with lock order session-then-holder.
 * Drive frames always carry [RobotMode.MANUAL]; the ViewModel's mode
 * snapshot and MANUAL-report gate arrive with the rewire.
 */
class DriveController(
    private val deviceRepo: DeviceRepository,
    private val getPairPsk: GetPairPskUseCase,
    private val toRobotCounter: ToRobotCounter,
    private val sendUC: SendSignedControlUseCase,
    private val takeManualUC: TakeManualUseCase,
    private val releaseManualUC: ReleaseManualUseCase,
    private val scope: CoroutineScope,
    private val tickMs: Long = DEFAULT_TICK_MS,
    private val idleHeartbeatMs: Long = DEFAULT_IDLE_HEARTBEAT_MS,
    private val stickActiveThreshold: Float = DEFAULT_STICK_ACTIVE_THRESHOLD,
) {
    private var supervisor: Job? = null
    private var currentDevice: PairedDevice? = null
    private var bound = false
    private var input: () -> Pair<Float, Float> = { 0f to 0f }

    private val sessionMutex = Mutex()
    private var sessionPsk: ByteArray? = null
    private var pskMissingFor: String? = null

    private var ticksSinceActivity = 0
    private var ticksSinceIdleSend = 0

    private val idleTickEvery: Int =
        maxOf(1, (idleHeartbeatMs / tickMs.coerceAtLeast(1)).toInt())

    init {
        start()
    }

    /**
     * Enters MANUAL (one `MANUAL_TAKE` on the shared counter) then streams
     * [input] (throttle to steering, each -1..1) at full rate. Take failure
     * still binds: there is no error channel until the rewire, and the
     * heartbeat below keeps the robot failsafe-fed either way.
     */
    fun bind(input: () -> Pair<Float, Float>) {
        val sup = supervisor ?: return
        this.input = input
        scope.launch(sup) {
            try {
                val dev = currentDevice ?: return@launch
                val prepared = sessionMutex.withLock { sessionCounterLocked(dev) }
                if (prepared != null) {
                    val (psk, counter) = prepared
                    takeManualUC(dev, psk, dev.era, counter)
                }
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // No error channel until the rewire: bind proceeds regardless.
            } finally {
                bound = true
            }
        }
    }

    /**
     * Best-effort MANUAL exit (the 2x byte-identical `MANUAL_RELEASE` lives
     * inside the use case) then idle zeroed heartbeats. Never throws:
     * every transmit error is swallowed — the robot's own command-stale
     * exit covers the stop.
     */
    fun unbind() {
        bound = false
        input = { 0f to 0f }
        val sup = supervisor ?: return
        scope.launch(sup) {
            try {
                val dev = currentDevice ?: return@launch
                val prepared = sessionMutex.withLock { sessionCounterLocked(dev) } ?: return@launch
                val (psk, counter) = prepared
                releaseManualUC(dev, psk, dev.era, counter)
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Best-effort by protocol: idle below resumes regardless.
            }
        }
    }

    /** Cancels the tick loop and any in-flight take/release; idempotent. */
    fun stop() {
        supervisor?.cancel()
        supervisor = null
    }

    private fun start() {
        if (supervisor != null) return
        val sup = SupervisorJob()
        supervisor = sup
        scope.launch(sup) { collectPaired() }
        scope.launch(sup) { tickLoop() }
    }

    private suspend fun collectPaired() {
        deviceRepo.observePaired().collect { paired ->
            currentDevice = paired
            if (paired == null) {
                sessionMutex.withLock {
                    sessionPsk = null
                    pskMissingFor = null
                    toRobotCounter.reset()
                }
                bound = false
                input = { 0f to 0f }
                ticksSinceActivity = 0
                ticksSinceIdleSend = 0
            }
        }
    }

    private suspend fun tickLoop() {
        while (true) {
            val dev = currentDevice
            if (dev == null) {
                delay(tickMs)
                continue
            }
            try {
                // A throwing input lambda must never kill the failsafe loop.
                val supplied = try {
                    if (bound) input() else (0f to 0f)
                } catch (_: Exception) {
                    0f to 0f
                }
                val throttle = supplied.first.coerceIn(-1f, 1f)
                val steering = supplied.second.coerceIn(-1f, 1f)
                val active = abs(throttle) > stickActiveThreshold ||
                    abs(steering) > stickActiveThreshold
                if (active) {
                    ticksSinceActivity = 0
                } else if (ticksSinceActivity < idleTickEvery) {
                    ticksSinceActivity++
                }
                val fullRate = active || ticksSinceActivity < idleTickEvery
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
                val prepared = sessionMutex.withLock { sessionCounterLocked(dev) }
                if (prepared != null) {
                    val (psk, counter) = prepared
                    sendUC(dev, ControlCommand(RobotMode.MANUAL, throttle, steering), psk, counter)
                }
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Best-effort heartbeat: a bad input or dead socket skips one tick.
            }
            delay(tickMs)
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

    private companion object {
        const val PAIR_PSK_BYTE_LENGTH = 16
        const val DEFAULT_TICK_MS = 20L
        const val DEFAULT_IDLE_HEARTBEAT_MS = 200L
        const val DEFAULT_STICK_ACTIVE_THRESHOLD = 0.02f
    }
}
