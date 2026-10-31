package com.desky.shared.features.control

import com.desky.shared.core.ControlCommand
import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.RobotMode
import com.desky.shared.features.session.SessionManager
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.concurrent.Volatile
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
 * dispatcher): tick counters and the bound flag are plain vars, session
 * allocation serializes inside [SessionManager].
 * Drive frames always carry [RobotMode.MANUAL]; the ViewModel's mode
 * snapshot and MANUAL-report gate arrive with the rewire.
 */
class DriveController(
    private val deviceRepo: DeviceRepository,
    private val sessionManager: SessionManager,
    private val sendUC: SendSignedControlUseCase,
    private val takeManualUC: TakeManualUseCase,
    private val releaseManualUC: ReleaseManualUseCase,
    private val scope: CoroutineScope,
    private val tickMs: Long = DEFAULT_TICK_MS,
    private val idleHeartbeatMs: Long = DEFAULT_IDLE_HEARTBEAT_MS,
    private val stickActiveThreshold: Float = DEFAULT_STICK_ACTIVE_THRESHOLD,
) {
    private var sessionScope: CoroutineScope? = null
    private var currentDevice: PairedDevice? = null
    private var bound = false
    private var input: () -> Pair<Float, Float> = { 0f to 0f }

    private var ticksSinceActivity = 0
    private var ticksSinceIdleSend = 0

    /**
     * App-background pause: while set, the tick loop sends nothing (no
     * vault/counter/socket use) and only delays — mirroring the legacy
     * screen's `backgroundPaused`. Set by [onBackground], cleared by
     * [onForeground]; both funnel from the platform lifecycle observers
     * that already drive `ConnectionManager`. Volatile: the flag is written
     * off the loop thread.
     */
    @Volatile
    private var paused = false

    private val idleTickEvery: Int =
        maxOf(1, (idleHeartbeatMs / tickMs.coerceAtLeast(1)).toInt())

    init {
        start()
    }

    /**
     * Latched emergency stop: one immediate stop frame now, then the tick
     * loop streams stop-flagged frames at full rate until [releaseEmergency].
     * The latch survives link loss and screen hides — only an explicit
     * release clears it. Volatile: set from the UI thread, read on the loop.
     */
    @Volatile
    private var estopLatched = false

    /**
     * App backgrounded: stop all sends until [onForeground]. Idempotent. */
    fun onBackground() {
        paused = true
    }

    /** App foregrounded: resume the tick loop. Idempotent. */
    fun onForeground() {
        paused = false
    }

    /**
     * Latches the emergency stop: one stop frame immediately, then the loop
     * streams the flagged variant at full rate until [releaseEmergency].
     * The bound stick closure is preserved — latched ticks ignore it and send
     * zeroed flagged frames, so release resumes driving with no rebind.
     * Best-effort like every fire-and-forget frame — a lost immediate frame
     * is covered by the latched stream. Idempotent while latched.
     */
    fun emergencyStop() {
        if (estopLatched) return
        estopLatched = true
        val session = sessionScope ?: return
        session.launch {
            try {
                val dev = currentDevice ?: return@launch
                val prepared = sessionManager.allocate(dev) ?: return@launch
                val (psk, counter) = prepared
                // Same wire shape as EmergencyStopUseCase (the legacy
                // screen's immediate-frame owner): zeroed MANUAL + ESTOP_FLAG.
                // Outcome discarded — the latched stream covers a loss.
                sendUC(dev, ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG), psk, counter)
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Covered by the latched stream on the next tick.
            }
        }
    }

    /** Clears a latched emergency stop; the loop falls back to cadence. */
    fun releaseEmergency() {
        estopLatched = false
    }

    /**
     * Enters MANUAL (one `MANUAL_TAKE` on the shared counter) then streams
     * [input] (throttle to steering, each -1..1) at full rate. Take failure
     * still binds: there is no error channel until the rewire, and the
     * heartbeat below keeps the robot failsafe-fed either way.
     */
    fun bind(input: () -> Pair<Float, Float>) {
        val session = sessionScope ?: return
        this.input = input
        session.launch {
            try {
                val dev = currentDevice ?: return@launch
                val prepared = sessionManager.allocate(dev)
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
        val session = sessionScope ?: return
        session.launch {
            try {
                val dev = currentDevice ?: return@launch
                val prepared = sessionManager.allocate(dev) ?: return@launch
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
        sessionScope?.cancel()
        sessionScope = null
    }

    private fun start() {
        if (sessionScope != null) return
        // Child scope supervised apart from the owner: stopping cancels the
        // loops without touching the owner.
        val session = CoroutineScope(scope.coroutineContext + SupervisorJob(scope.coroutineContext[Job]))
        sessionScope = session
        session.launch { collectPaired() }
        session.launch { tickLoop() }
    }

    private suspend fun collectPaired() {
        deviceRepo.observePaired().collect { paired ->
            currentDevice = paired
            if (paired == null) {
                bound = false
                input = { 0f to 0f }
                ticksSinceActivity = 0
                ticksSinceIdleSend = 0
            }
        }
    }

    private suspend fun tickLoop() {
        while (true) {
            if (paused) {
                delay(tickMs)
                continue
            }
            val dev = currentDevice
            if (dev == null) {
                delay(tickMs)
                continue
            }
            try {
                if (estopLatched) {
                    // Stop pins full rate like active driving: a lost frame
                    // costs one tick (20 ms), never an idle hole (200 ms).
                    ticksSinceActivity = 0
                    sendFrame(dev, 0f, 0f, ESTOP_FLAG)
                } else {
                    val (throttle, steering) = sampleInput()
                    if (!dueForSend(throttle, steering)) {
                        delay(tickMs)
                        continue
                    }
                    sendFrame(dev, throttle, steering)
                }
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Best-effort heartbeat: a bad input or dead socket skips one tick.
            }
            delay(tickMs)
        }
    }

    /** Stick input in -1..1, snapped to center below the drive threshold. */
    private fun sampleInput(): Pair<Float, Float> {
        // A throwing input lambda must never kill the failsafe loop.
        val supplied = try {
            if (bound) input() else (0f to 0f)
        } catch (_: Exception) {
            0f to 0f
        }
        return supplied.first.coerceIn(-1f, 1f) to supplied.second.coerceIn(-1f, 1f)
    }

    /**
     * True when this tick sends a frame. Full rate while the stick is
     * active or inside the activity grace; otherwise the idle heartbeat
     * cadence. Skipped ticks allocate no counter.
     */
    private fun dueForSend(throttle: Float, steering: Float): Boolean {
        val active = abs(throttle) > stickActiveThreshold ||
            abs(steering) > stickActiveThreshold
        if (active) {
            ticksSinceActivity = 0
        } else if (ticksSinceActivity < idleTickEvery) {
            ticksSinceActivity++
        }
        if (active || ticksSinceActivity < idleTickEvery) {
            ticksSinceIdleSend = 0
            return true
        }
        ticksSinceIdleSend++
        if (ticksSinceIdleSend < idleTickEvery) return false
        ticksSinceIdleSend = 0
        return true
    }

    /** One signed `MANUAL` drive frame on the shared counter. */
    private suspend fun sendFrame(dev: PairedDevice, throttle: Float, steering: Float, flags: Int = 0) {
        val prepared = sessionManager.allocate(dev) ?: return
        val (psk, counter) = prepared
        sendUC(dev, ControlCommand(RobotMode.MANUAL, throttle, steering, flags), psk, counter)
    }

    private companion object {
        const val DEFAULT_TICK_MS = 20L
        const val DEFAULT_IDLE_HEARTBEAT_MS = 200L
        const val DEFAULT_STICK_ACTIVE_THRESHOLD = 0.02f
    }
}
