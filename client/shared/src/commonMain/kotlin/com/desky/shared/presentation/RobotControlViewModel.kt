package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.ControlCommand
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.ToRobotCounter
import com.desky.shared.domain.ESTOP_FLAG
import com.desky.shared.domain.AliveUseCase
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ExpressionEntry
import com.desky.shared.domain.FetchExpressionsUseCase
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.FromRobotCounter
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.ObserveRemoteDisconnectUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.PairRefreshUseCase
import com.desky.shared.domain.ReleaseManualUseCase
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.TakeManualUseCase
import com.desky.shared.domain.Telemetry
import com.desky.shared.domain.TriggerExpressionUseCase
import com.desky.shared.domain.UdpSocketBinder
import com.desky.shared.domain.UdpSocketManager
import com.desky.shared.domain.hexToBytes
import kotlinx.coroutines.CancellationException
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
import kotlin.concurrent.Volatile
import kotlin.math.abs

/** Link state of the control screen. */
enum class Connectivity {
    Disconnected,
    Unpaired,
    Connected,
}

/** Gated-tick session probe outcome (see `RobotControlViewModel.probeSessionLocked`). */
private enum class SessionHealth {
    Healthy,
    MissingPsk,
    Expired,
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
 * `ALIVE` keepalive period: while paired (session ready) the send loop
 * transmits one signed `ALIVE` (`0x55`) this often on its OWN tick clock
 * ([ticksSinceAlive]), independent of the drive idle heartbeat above. 2 Hz
 * is far inside the robot's 5 s alive window
 * (`kAliveMs` — a 10x margin), so telemetry keeps flowing unicast even
 * when the sticks sit centered and the drive loop idles to ~5 Hz. A valid
 * `ALIVE` consumes the shared counter exactly like drive (monotonic
 * preserved). Fire-and-forget: the robot never replies — steady telemetry
 * is the ACK, and send errors never touch the error banner.
 */
const val ALIVE_INTERVAL_MS = 500L

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
    /**
     * Robot-reported MANUAL hold (telemetry `statusFlags` bit 4). Paired no
     * longer implies drivable: the drive send loop only transmits while
     * this is true (drive frames outside MANUAL are ignored robot-side).
     * Set by [onTakeManual], cleared by [onReleaseManual] (both converge
     * via telemetry).
     */
    val manualMode: Boolean = false,
    /**
     * Robot-owned expression catalog (fetched on screen entry via
     * `EXPR_LIST_REQ`, cached for the ViewModel lifecycle, refetched on
     * return — see [onForeground]). Empty until the first fetch lands; a
     * tap sends one `EXPR_TRIGGER` while MANUAL is reported (see
     * [onTriggerExpression]).
     */
    val expressions: List<ExpressionEntry> = emptyList(),
    /** True while an expression-catalog fetch is in flight. */
    val expressionsLoading: Boolean = false,
)

/**
 * Drives the robot-control screen (pairing/auth Task 9: every frame is an
 * authenticated `DRIVE` datagram).
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
 * Drive session: after the first pairing the loop loads the pair PSK via
 * [GetPairPskUseCase] ONCE and caches it in memory, seeding the shared
 * [ToRobotCounter] from the Room row (`lastCounter`), so the first wire
 * packet uses counter 1 after a fresh pair. Each send allocates the next
 * counter from the holder (which persists to Room every 128th allocation).
 * A missing PSK surfaces [Connectivity.Unpaired] + "Please re-pair to
 * drive" and sends NOTHING (never legacy frames); the miss is latched per
 * device so the vault is still read at most once (the idle loop does not
 * retry).
 *
 * A later unpair pauses the send loop until re-pair (the loop re-reads
 * [currentDevice] every tick) and drops the cached PSK plus both
 * counter-holder eras (send + receive), so the next pairing reloads both
 * fresh (one more vault read for the new session). The session mutex serializes counter allocation between the
 * loop tick and the immediate e-stop frame so both share one strictly
 * increasing counter sequence; the actual network send always happens
 * OUTSIDE the lock so a stalled send never blocks the other path.
 * [tickMs] is a constructor parameter so tests can drive virtual time.
 *
 * Robot alive rule (`ALIVE` / `DISCONNECT`, null [aliveUC] keeps both
 * inert): once the session is ready the loop also transmits one signed
 * `ALIVE` immediately and then every [ALIVE_INTERVAL_MS] on its own tick
 * clock (independent of the drive idle heartbeat above) — each `ALIVE`
 * allocates from the same shared [ToRobotCounter], so the monotonic sequence
 * is preserved and telemetry keeps flowing unicast inside the robot's 5 s
 * alive window. `ALIVE` results never touch [_state] (telemetry is the
 * ACK). [onBackground] sends `DISCONNECT` 2x fire-and-forget (pair kept)
 * before pausing; [onForeground] forces the `ALIVE` clock due so the next
 * tick knocks immediately.
 *
 * Disconnected gating: while [RobotControlUiState.connectivity] is
 * [Connectivity.Disconnected], [onStick], [onEmergencyStop], and
 * [onReleaseEmergency] are no-ops (early return — no state change, latched
 * error kept). The drive send loop and the `ALIVE` knock keep running
 * underneath; the first telemetry promotes back to [Connectivity.Connected]
 * via the existing path and re-enables input.
 *
 * Manual gating: paired no longer implies drivable. Drive frames are only
 * transmitted while [RobotControlUiState.manualMode] is true (the
 * robot-reported MANUAL hold, telemetry `statusFlags` bit 4, on a
 * [Connectivity.Connected] link — frames outside MANUAL are ignored
 * robot-side). Ticks without the hold probe session health WITHOUT
 * allocating (the missing-PSK repair copy still surfaces pre-link; the
 * dense counter sequence is untouched) and idle until MANUAL is reported.
 * The `ALIVE` knock keeps going underneath (it is what keeps telemetry — and
 * the MANUAL report — flowing). [onTakeManual] enters MANUAL (one
 * `MANUAL_TAKE` per user action), [onReleaseManual] exits it (best-effort 2x
 * `MANUAL_RELEASE`); both converge via telemetry like every other mode edge.
 *
 * Expression picker: the send loop fetches the robot-owned catalog once
 * per screen entry (one `EXPR_LIST_REQ`, lifecycle-cached in state,
 * refetched on return) and [onTriggerExpression] plays one entry per tap
 * (one `EXPR_TRIGGER`, MANUAL-gated like the sticks, silent robot-side).
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
    private val getPairPskUC: GetPairPskUseCase,
    private val udp: UdpSocketManager,
    private val toRobotCounter: ToRobotCounter,
    private val fromRobotCounter: FromRobotCounter,
    private val tickMs: Long = 20,
    /**
     * Tier-1 silent auto-pair-refresh trigger (stub this pass — see [maybePairRefresh]):
     * null in tests/legacy call sites (pair-refresh checking inert); production
     * DI passes the shared [PairRefreshUseCase]. Follow-up tunes the background
     * launch + session-cache refresh so the rotation fires proactively at
     * [ToRobotCounter.needsPairRefresh] with zero send-loop stall.
     */
    private val pairRefreshUC: PairRefreshUseCase? = null,
    /**
     * Robot alive rule keepalive (see [ALIVE_INTERVAL_MS]): null in
     * tests/legacy call sites (`ALIVE`/`DISCONNECT` inert — drive counters
     * and background behavior stay exactly as before); production DI
     * passes the shared [AliveUseCase]. When present the send loop
     * transmits one signed `ALIVE` immediately on the first paired tick
     * (session ready) and then every [ALIVE_INTERVAL_MS] on its own tick
     * clock, and [onBackground] sends `DISCONNECT` 2x fire-and-forget.
     */
    private val aliveUC: AliveUseCase? = null,
    /**
     * AP-rejoin signal (see [UdpSocketBinder.networkAvailable]): null in
     * tests/legacy call sites and on JVM/iOS (no binder — iOS has no
     * `NWPathMonitor` binder yet, so rejoin there relies on the steady 2 Hz
     * cadence); production Android DI passes the shared `WifiSocketBinder`.
     * When present the VM collects it and forces the `ALIVE` clock due, so
     * the next tick knocks immediately like [onForeground] (without
     * clearing the background pause). The Android 1 s debounce suffices —
     * no ViewModel debounce: repeated due-flags before the next tick
     * collapse idempotently to one `ALIVE`.
     */
    private val socketBinder: UdpSocketBinder? = null,
    /**
     * Broken-row convergence (nav-guard Task 2): null in tests/legacy call
     * sites (missing/exhausted surfaces stay local-only); production DI
     * passes the shared [ForgetDeviceUseCase]. On the missing-PSK and
     * session-exhausted paths the VM fires forget-once per device (latched
     * like [pskMissingFor], so per-tick clears never happen) — the repo
     * emits null and the global [SessionGateViewModel] fence navigates to
     * Pairing. `UNPAIR` best-effort inside is already null-safe (missing PSK
     * or exhausted counter skips the send). Disconnected (link-down) never
     * forgets — only Unpaired/missing/exhausted converge.
     */
    private val forgetUC: ForgetDeviceUseCase? = null,
    /**
     * Core-originated `DISCONNECT` eviction (physical pair-button press):
     * null in tests/legacy call sites (remote evictions inert — a cleared
     * robot then relies on the existing telemetry-timeout/missing-PSK
     * convergence); production DI passes the shared
     * [ObserveRemoteDisconnectUseCase]. When present the VM collects its
     * verified-eviction flow: the use case already ran the local forget
     * (Room row + vault + both counter-holder resets, no reply), so the
     * repo emits null and the existing unpair branch pauses the send loop +
     * surfaces Unpaired while the session gate navigates to Pairing.
     * Collection body is correlation logging only.
     */
    private val remoteDisconnectUC: ObserveRemoteDisconnectUseCase? = null,
    /**
     * Manual take/release (see `shared/protocol/README.md` §"Pairing +
     * authenticated drive v1", MANUAL take/release bullets): null in tests/legacy call sites (take/release intents
     * inert — the drive loop still gates on the reported MANUAL bit, so a
     * legacy graph simply never becomes drivable); production DI passes the
     * shared [TakeManualUseCase] + [ReleaseManualUseCase]. [onTakeManual]
     * sends one `MANUAL_TAKE` per user action, [onReleaseManual] sends
     * `MANUAL_RELEASE` 2x byte-identical best-effort — both allocate from
     * the shared [ToRobotCounter] via the session helper, preserving the
     * monotonic sequence with drive/`ALIVE`.
     */
    private val takeManualUC: TakeManualUseCase? = null,
    private val releaseManualUC: ReleaseManualUseCase? = null,
    /**
     * Expression catalog (fetch on screen entry, trigger per tap): null in
     * tests/legacy call sites (the picker stays empty and trigger intents
     * inert); production DI passes the shared [FetchExpressionsUseCase] +
     * [TriggerExpressionUseCase]. [onTriggerExpression] sends one
     * `EXPR_TRIGGER` per tap while MANUAL is reported; the fetch runs once
     * per screen entry with the cache living in [_state].
     */
    private val fetchExpressionsUC: FetchExpressionsUseCase? = null,
    private val triggerExpressionUC: TriggerExpressionUseCase? = null,
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

    /** Paired-loop ticks per `ALIVE` keepalive ([ALIVE_INTERVAL_MS] / [tickMs]). */
    private val aliveTickEvery: Int =
        maxOf(1, (ALIVE_INTERVAL_MS / tickMs.coerceAtLeast(1)).toInt())

    /**
     * Send-loop ticks since the last `ALIVE` keepalive — the 2 Hz clock.
     * Incremented once per paired, non-backgrounded send-loop iteration
     * (virtual-time friendly, like [ticksSinceTelemetry]) and fully
     * independent of the drive idle heartbeat above: `ALIVE` goes out even
     * on ticks where the drive loop idles to a skip. Starts due (==
     * [aliveTickEvery]) so the first paired tick — vault + Room + socket
     * done, session ready — transmits immediately; [onForeground], the AP-
     * rejoin collector, and the unpair reset force it due again for the same
     * immediate reason. Runs
     * on `viewModelScope` (Main), so a plain var suffices — see
     * [ticksSinceTelemetry].
     */
    private var ticksSinceAlive: Int = aliveTickEvery

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
     * Tier-1 pair-refresh in-flight guard for [maybePairRefresh]: while true a rotation
     * attempt is outstanding (or recently finished) — the per-tick check
     * skips re-firing. Runs on `viewModelScope` (Main), so a plain var
     * suffices — see [ticksSinceTelemetry].
     */
    private var pairRefreshInFlight = false
    /**
     * Expression-catalog fetch gating for [maybeFetchExpressions]: while
     * true the next paired send-loop tick launches one background fetch
     * (single `EXPR_LIST_REQ`, no retry). Set on init (first session
     * fetches on entry) and on every [onForeground] (returning to the
     * screen refetches); cleared synchronously when the fetch launches so
     * ticks never stack a second one. Runs on `viewModelScope` (Main), so
     * plain vars suffice — see [ticksSinceTelemetry].
     */
    private var expressionsDue = true
    /**
     * Expression-catalog fetch in-flight guard: while true a fetch attempt
     * is outstanding — the per-tick check skips re-firing until it
     * completes (success caches into [_state], failure stays silent: the
     * next screen entry heals).
     */
    private var expressionsFetchInFlight = false
    /**
     * Device id whose vault lookup already came back empty. The loop idles
     * for it (no per-tick vault reads — the vault is read at most once per
     * session, missing or not); cleared on unpair so a re-pair reloads.
     */
    private var pskMissingFor: String? = null
    /**
     * Forget-once latch for the broken-row convergence above: device id
     * whose missing/exhausted forget already fired. Set synchronously
     * before the `viewModelScope` launch so per-tick calls collapse to one
     * repo clear; cleared on unpair (same place as [pskMissingFor]) so a
     * re-pair can converge again.
     */
    private var forgetFiredFor: String? = null

    /**
     * Correlation markers (log-only, no behavior change): sessionReady /
     * firstAliveSent / firstTelemetrySeen, each once per session, reset on
     * unpair. Wall-clock comes from the platform log timestamp (logcat /
     * stdout); the robot side logs the same names with `millis()` + boot-id,
     * so a session's app/robot timelines join on the shared marker names +
     * counter values. Never log key material (PSK/nonce); counters/era/mode
     * are safe.
     */
    private var loggedSessionReady = false
    private var loggedFirstAlive = false
    private var loggedFirstTelemetry = false

    /**
     * App-side correlation marker sink. commonMain has no shared logger
     * (androidMain uses `android.util.Log`; commonMain has none), so plain
     * `println` — visible in logcat, JVM stdout, and the iOS console —
     * with a `RobotControl` tag prefix.
     */
    private fun logMarker(msg: String) {
        println("RobotControl $msg")
    }

    init {
        // AP rejoin (Android `WifiSocketBinder.onAvailable`, debounced):
        // force the `ALIVE` clock due so the next send-loop tick knocks
        // immediately, same as [onForeground] but without resuming a
        // background pause. Unconditional by design: while unpaired the
        // unpair branch already holds the clock due, and while backgrounded
        // the loop skips `ALIVE` anyway — the flag simply waits for the next
        // foreground tick. Idempotent: a flap burst before the next tick
        // collapses to one `ALIVE` (the Android 1 s debounce already drops
        // the connect/re-validate burst).
        socketBinder?.let { binder ->
            viewModelScope.launch {
                binder.networkAvailable.collect {
                    ticksSinceAlive = aliveTickEvery
                }
            }
        }
        viewModelScope.launch {
            deviceRepo.observePaired().collect { paired ->
                currentDevice.value = paired
                if (paired == null) {
                    _state.update { it.copy(connectivity = Connectivity.Unpaired) }
                }
            }
        }
        // Core-originated eviction: the use case owns verify + local forget;
        // this collector only logs the correlation marker (repo-null drives
        // the pause/unpair/gate convergence above). Null (tests/legacy)
        // collects nothing.
        remoteDisconnectUC?.let { evictions ->
            viewModelScope.launch {
                evictions.observe().collect {
                    logMarker("remoteDisconnectCleared")
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
                    if (!loggedFirstTelemetry) {
                        loggedFirstTelemetry = true
                        logMarker("firstTelemetrySeen mode=${telemetry.mode}")
                    }
                    val prev = _state.value
                    // UI path (gated, expensive): copy telemetry/mode/manual only
                    // when due or high-value, so a 15 Hz stream recomposes
                    // at ~5 Hz. Promotion from non-Connected always paints
                    // (fresh packet after silence must be visible); edges
                    // (mode/cliff/driving/manual flips) paint immediately even
                    // inside the window. Otherwise skip [_state] entirely —
                    // already Connected means "ensure Connected" is a no-op.
                    val highValueChange = prev.telemetry == null ||
                        prev.mode != telemetry.mode ||
                        prev.telemetry.mode != telemetry.mode ||
                        prev.telemetry.cliff != telemetry.cliff ||
                        prev.telemetry.driving != telemetry.driving ||
                        prev.telemetry.manual != telemetry.manual
                    val timeDue = ticksSinceTelemetryUi >= telemetryUiEvery
                    if (prev.connectivity != Connectivity.Connected || highValueChange || timeDue) {
                        ticksSinceTelemetryUi = 0
                        _state.update {
                            it.copy(
                                telemetry = telemetry,
                                mode = telemetry.mode,
                                manualMode = telemetry.manual,
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
                            forgetFiredFor = null
                            toRobotCounter.reset()
                            fromRobotCounter.reset()
                        }
                        pairRefreshInFlight = false
                        // A re-pair advertises a fresh catalog: the next
                        // session fetches on entry (the stale cache stays
                        // visible until it lands).
                        expressionsDue = true
                        ticksSinceTelemetry = 0
                        ticksSinceActivity = 0
                        ticksSinceIdleSend = 0
                        ticksSinceTelemetryUi = telemetryUiEvery
                        // Re-pair knocks immediately (first paired tick due).
                        ticksSinceAlive = aliveTickEvery
                        // New session: correlation markers fire once again.
                        loggedSessionReady = false
                        loggedFirstAlive = false
                        loggedFirstTelemetry = false
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
                    // `ALIVE` keepalive (own 2 Hz clock, before the drive
                    // idle gate so it stays independent of it): due ticks
                    // allocate one shared counter via the session helper
                    // and transmit fire-and-forget — the result is
                    // deliberately ignored (telemetry is the ACK; a lost
                    // keepalive never touches the error banner). A null
                    // allocation (missing PSK / exhausted era) skips
                    // silently: the drive section below owns both surfaces.
                    // Null [aliveUC] (tests/legacy graphs) skips the alloc
                    // too, so legacy counter sequences are untouched.
                    // Increment-first (same idiom as the telemetry timeout
                    // above), so the steady period is exactly
                    // [aliveTickEvery] ticks: due at tick N means the next
                    // due tick is N + aliveTickEvery.
                    if (ticksSinceAlive < aliveTickEvery) {
                        ticksSinceAlive++
                    }
                    val aliveDue = ticksSinceAlive >= aliveTickEvery
                    if (aliveDue) {
                        ticksSinceAlive = 0
                    }
                    if (aliveDue && aliveUC != null) {
                        val alivePrepared = sessionMutex.withLock { sessionCounterLocked(dev) }
                        if (alivePrepared != null) {
                            val (alivePsk, aliveCounter) = alivePrepared
                            aliveUC(dev, alivePsk, dev.era, aliveCounter)
                            if (!loggedFirstAlive) {
                                loggedFirstAlive = true
                                logMarker("firstAliveSent era=${dev.era} ctr=$aliveCounter")
                            }
                        }
                    }
                    // Expression catalog (own once-per-entry clock, before
                    // the drive gate so the picker fills regardless of the
                    // MANUAL hold): due ticks launch one background fetch —
                    // never inline, the response wait must not stall the
                    // 20 ms send loop or the robot's 500 ms failsafe trips.
                    maybeFetchExpressions(dev)
                    val snapshot = _state.value
                    // Idle heartbeat: centered + e-stop-clear + no activity
                    // in the last IDLE_HEARTBEAT_MS idles to ~5 Hz; anything
                    // else sends every tick. Telemetry counting above stays
                    // on wall ticks (it already ran), and skipped ticks
                    // allocate NO counter (ToRobotCounter.next only runs on a
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
                    // Tier-1 pair-refresh probe first (stub: no-op unless the era is
                    // within PAIR_REFRESH_MARGIN of the gate — see maybePairRefresh).
                    maybePairRefresh(dev)
                    val result: Outcome<Unit>? = if (!drivable(snapshot)) {
                        // Gated: linked-but-not-MANUAL (or pre-link) — drive
                        // frames would be ignored robot-side, so probe session
                        // health WITHOUT allocating (nothing transmits while
                        // gated; the dense counter sequence is untouched) and
                        // idle until MANUAL is reported. The `ALIVE` knock
                        // above is unaffected: it is what keeps telemetry —
                        // and the MANUAL report — flowing.
                        when (sessionMutex.withLock { probeSessionLocked(dev) }) {
                            SessionHealth.MissingPsk -> missingPsk(dev)
                            SessionHealth.Expired -> sessionExpired(dev)
                            SessionHealth.Healthy -> null
                        }
                    } else {
                        // Allocate (psk, counter) under the session mutex, then
                        // send OUTSIDE the lock: udp.send has unbounded latency
                        // and must never stall the e-stop path behind a loop tick.
                        val prepared = sessionMutex.withLock { sessionCounterLocked(dev) }
                        if (prepared == null) {
                            // Exhaustion (fail-closed counter gate) vs. missing
                            // PSK: distinct copy — the former needs an explicit
                            // re-pair, and no UNPAIR counter is consumed after it
                            // (ForgetDeviceUseCase skips the send on null).
                            if (toRobotCounter.isExhausted()) sessionExpired(dev) else missingPsk(dev)
                        } else {
                            val (psk, next) = prepared
                            sendUC(dev, cmd, psk, next)
                        }
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
                        null ->
                            Unit
                    }
                    delay(tickMs)
                }
            }
        }
    }

    /**
     * Session load + counter allocation shared by the loop tick and the
     * immediate e-stop frame, so both consume one strictly increasing
     * sequence from the shared [ToRobotCounter]. Must hold [sessionMutex].
     * Returns null when there is no PSK or no counter available (caller
     * reports [missingPsk] for the loop tick, or skips the immediate frame
     * for e-stop — the loop sends the latched variant on its next tick).
     * Never performs network I/O: callers send OUTSIDE the lock so a
     * stalled send cannot block the other path's allocation.
     * The holder's fail-closed gate ([ToRobotCounter.EXHAUSTION_GATE]) refuses
     * to allocate at/above the gate (latching [ToRobotCounter.isExhausted] —
     * 0 is never valid on the wire and the wrap-adjacent region stays
     * unused); callers tell exhaustion apart from a missing PSK via the
     * latch and report [sessionExpired] instead of [missingPsk].
     */
    private suspend fun sessionCounterLocked(dev: PairedDevice): Pair<ByteArray, UInt>? {
        val psk = loadSessionPskLocked(dev) ?: return null
        // Seed from Room on a fresh era only; a live era is a no-op, so the
        // watermark (and the negotiated persist cadence + pair-refresh era) can
        // never be rewound mid-session.
        toRobotCounter.ensure(dev.id, dev.lastCounter, dev.persistEvery, dev.era)
        val next = toRobotCounter.next() ?: return null
        if (!loggedSessionReady) {
            loggedSessionReady = true
            logMarker("sessionReady id=${dev.id} era=${dev.era} ctr=$next")
        }
        return psk to next
    }

    /**
     * Vault PSK load + cache shared by [sessionCounterLocked] and
     * [probeSessionLocked]. Must hold [sessionMutex]. Null when the vault
     * has no entry for this device or the entry is undecodable (both
     * latched per device: at most one vault read per session). Cancellation
     * propagates.
     */
    private suspend fun loadSessionPskLocked(dev: PairedDevice): ByteArray? {
        var psk = sessionPsk
        if (psk == null) {
            // Missing-PSK latch: the loop idles for this device instead of
            // reading the vault every tick (at most one vault read per
            // session, keyless or not).
            if (pskMissingFor == dev.id) return null
            val hex = getPairPskUC()
            if (hex == null) {
                pskMissingFor = dev.id
                return null
            }
            // Vault-hex guard: at-rest corruption (odd length / non-hex) throws
            // inside hexToBytes — without this the send-loop coroutine dies on
            // the first tick (ALIVE, drive, e-stop all share this choke).
            // Wrong-length valid hex (e.g. "abcd" -> 2 bytes) is rejected too:
            // encodeSigned requires the 16-byte pair PSK. All undecodable
            // entries reuse the existing missing-PSK path (latched, one vault
            // read per session; drive surfaces the re-pair copy, ALIVE and
            // e-stop stay silent).
            val decoded = try {
                hexToBytes(hex)
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                null
            }
            if (decoded == null || decoded.size != 16) {
                logMarker("sessionPskUndecodable")
                pskMissingFor = dev.id
                return null
            }
            psk = decoded
            sessionPsk = psk
        }
        return psk
    }

    /**
     * Session health for gated (non-drivable) loop ticks. Must hold
     * [sessionMutex]. Loads/caches the PSK exactly like the send path but
     * allocates NO counter — nothing transmits while gated, so the dense
     * sequence is untouched. A missing/undecodable PSK reports
     * [SessionHealth.MissingPsk] (the caller surfaces the same repair copy
     * as the drive path, so keyless pairs still converge); a latched
     * exhaustion reports [SessionHealth.Expired]. Cancellation propagates.
     */
    private suspend fun probeSessionLocked(dev: PairedDevice): SessionHealth {
        if (loadSessionPskLocked(dev) == null) return SessionHealth.MissingPsk
        if (toRobotCounter.isExhausted()) return SessionHealth.Expired
        return SessionHealth.Healthy
    }

    /**
     * Drivability: paired no longer implies drivable. Drive frames transmit
     * only on a [Connectivity.Connected] link while the robot reports the
     * MANUAL hold ([RobotControlUiState.manualMode], telemetry `statusFlags`
     * bit 4) — frames outside MANUAL are ignored robot-side.
     */
    private fun drivable(snapshot: RobotControlUiState): Boolean =
        snapshot.connectivity == Connectivity.Connected && snapshot.manualMode

    /**
     * Tier-1 silent auto-pair-refresh trigger STUB (this pass): called once per
     * send-loop tick before counter allocation. When the shared
     * [ToRobotCounter] reports [ToRobotCounter.needsPairRefresh] (counter within
     * [ToRobotCounter.PAIR_REFRESH_MARGIN] of the fail-closed gate) the rotation
     * should fire silently in the background — no UI except a debug log —
     * and on success refresh [sessionPsk] from the vault (the [PairRefreshUseCase]
     * already persisted vault + Room + holder; this VM's in-memory session
     * cache is the only stale copy) so the next drive uses the new key
     * gaplessly.
     *
     * STUB SCOPE: this pass only probes [needsPairRefresh] (one mutex read — zero
     * behavior change when false or when [pairRefreshUC] is null, so all 107
     * existing tests stay green) and records the trigger; it does NOT
     * launch the exchange yet. FOLLOW-UP (auto-trigger tuning): background-
     * launch `pairRefreshUC()` from `viewModelScope` guarded by [pairRefreshInFlight]
     * (never inline — the 3x 1 s ACK wait must not stall the 20 ms send
     * loop or the robot's 500 ms failsafe trips), refresh [sessionPsk]
     * under [sessionMutex] on success, clear [pairRefreshInFlight] + back off on
     * failure (Tier-2 repair copy owns the gate — never surface pair-refresh
     * failure to the screen).
     */
    private suspend fun maybePairRefresh(dev: PairedDevice) {
        val pairRefresh = pairRefreshUC ?: return
        if (pairRefreshInFlight) return
        if (!toRobotCounter.needsPairRefresh()) return
        // Trigger point reached (counter within PAIR_REFRESH_MARGIN of the gate):
        // park here until the follow-up wires the background launch above.
        // Intentionally silent — no state/error change (a debug log lands
        // with the launch patch, where a log backend exists).
        pairRefreshInFlight = true
    }

    /**
     * Expression-catalog fetch trigger (once per screen entry): called on
     * every paired, non-backgrounded send-loop tick. When [expressionsDue]
     * (set on init + every [onForeground]) and no fetch is in flight, one
     * background fetch launches — allocate under the session mutex, send +
     * await outside it (the up-to-[EXPR_LIST_TIMEOUT_MS] response wait must
     * never stall the send loop). Success caches the catalog into [_state]
     * for the ViewModel lifecycle; failure stays silent (no error banner —
     * the next screen entry refetches and heals). Null [fetchExpressionsUC]
     * (tests/legacy graphs) leaves the picker empty.
     */
    private fun maybeFetchExpressions(dev: PairedDevice) {
        val fetch = fetchExpressionsUC ?: return
        if (!expressionsDue || expressionsFetchInFlight) return
        expressionsDue = false
        expressionsFetchInFlight = true
        _state.update { it.copy(expressionsLoading = true) }
        viewModelScope.launch {
            try {
                // One allocation under the session mutex (same order as the
                // loop tick: session-then-holder), sent outside it.
                val prepared = sessionMutex.withLock { sessionCounterLocked(dev) } ?: return@launch
                val (psk, counter) = prepared
                when (val result = fetch(dev, psk, dev.era, counter)) {
                    is Outcome.Success -> _state.update {
                        it.copy(expressions = result.data, expressionsLoading = false)
                    }
                    is Outcome.Failure -> _state.update { it.copy(expressionsLoading = false) }
                }
            } finally {
                expressionsFetchInFlight = false
            }
        }
    }

    private fun missingPsk(dev: PairedDevice): Outcome.Failure {
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
        maybeForget(dev)
        return Outcome.Failure(message)
    }

    /**
     * Tier-2 exhaustion surface: the shared [ToRobotCounter] hit
     * [EXHAUSTION_GATE][ToRobotCounter.EXHAUSTION_GATE] and refuses to allocate
     * (fail-closed — no counter is consumed, so the loop and
     * [ForgetDeviceUseCase]'s `UNPAIR` both skip their sends). Distinct copy
     * from [missingPsk]: the pair exists, the session just aged out after
     * long use — an explicit re-pair starts a fresh era.
     */
    private fun sessionExpired(dev: PairedDevice): Outcome.Failure {
        val message = "Session expired after long use — please re-pair"
        val current = _state.value
        if (current.connectivity != Connectivity.Unpaired || current.error != message) {
            _state.update {
                it.copy(
                    connectivity = Connectivity.Unpaired,
                    error = message,
                )
            }
        }
        maybeForget(dev)
        return Outcome.Failure(message)
    }

    /**
     * Broken-row convergence fire-once: clears the Room row (vault entry
     * included) via [forgetUC] so the repo emits null and the session gate
     * fires. Latched per device id — per-tick missing/exhausted calls
     * collapse to a single clear. Null [forgetUC] (tests/legacy graphs) is
     * a no-op. Failures surface as [Outcome.Failure] inside the use case,
     * never as throws (except cancellation), so the launched child just
     * ignores the result — the local Unpaired copy above already surfaced.
     */
    private fun maybeForget(dev: PairedDevice) {
        val forget = forgetUC ?: return
        if (forgetFiredFor == dev.id) return
        forgetFiredFor = dev.id
        viewModelScope.launch {
            try {
                forget()
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
                // Swallowed: the Unpaired copy above already surfaced; a
                // failed clear retries on the next session (latch resets on
                // unpair, and the copy persists regardless).
            }
        }
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
     *
     * Disconnected gate: while [RobotControlUiState.connectivity] is
     * [Connectivity.Disconnected] this is a no-op (early return — no state
     * change, latched error kept). The `ALIVE` knock keeps going underneath
     * and the first telemetry re-enables via the existing promotion path.
     */
    fun onStick(throttle: Float, steering: Float) {
        if (_state.value.connectivity == Connectivity.Disconnected) return
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
     *
     * Disconnected gate: while [RobotControlUiState.connectivity] is
     * [Connectivity.Disconnected] this is a no-op (early return — no latch,
     * no frame, latched error kept). E-stop included: with no link the
     * robot's ~500 ms stale-command failsafe is already stopping it, and a
     * frame the robot drops as stale would only fake confidence.
     */
    fun onEmergencyStop() {
        if (_state.value.connectivity == Connectivity.Disconnected) return
        _state.update { it.copy(eStopped = true, throttle = 0f, steering = 0f) }
        viewModelScope.launch {
            val paired = currentDevice.value ?: return@launch
            // Allocate under the session mutex, send outside it: an
            // in-flight loop send never blocks this immediate frame's
            // allocation (and vice versa).
            val prepared = sessionMutex.withLock { sessionCounterLocked(paired) }
            if (prepared == null) {
                // Exhausted era: surface the repair copy here too (the loop
                // tick would on its next pass); a missing PSK stays silent —
                // the loop owns that copy.
                if (toRobotCounter.isExhausted()) sessionExpired(paired)
                return@launch
            }
            val (psk, next) = prepared
            val result = emergencyUC(paired, psk, next)
            if (result is Outcome.Failure) {
                _state.update { it.copy(error = result.reason) }
            }
        }
    }

    /**
     * Releases a latched e-stop. Disconnected gate (same as [onStick] /
     * [onEmergencyStop]): while [RobotControlUiState.connectivity] is
     * [Connectivity.Disconnected] this is a no-op, so a latch held across
     * a link loss stays latched until telemetry re-enables.
     */
    fun onReleaseEmergency() {
        if (_state.value.connectivity == Connectivity.Disconnected) return
        _state.update { it.copy(eStopped = false) }
    }

    /**
     * Enters MANUAL: one signed `MANUAL_TAKE` frame goes out immediately via
     * [takeManualUC] (sharing the session counter with the loop tick under
     * [sessionMutex]). Entry is deliberate — exactly one frame per user
     * action, never repeated. The robot reports back via the telemetry
     * MANUAL bit, which gates the drive loop and paints [RobotControlUiState.manualMode].
     *
     * Disconnected gate (same as [onStick] / [onEmergencyStop]): while
     * [RobotControlUiState.connectivity] is [Connectivity.Disconnected] this
     * is a no-op. Already-manual is a no-op too (repeats add nothing — no
     * counter consumed). When no session is loaded yet the frame is skipped
     * silently except exhaustion, which surfaces the repair copy like the
     * immediate e-stop path; a transport failure surfaces its reason (entry
     * was an explicit user action).
     */
    fun onTakeManual() {
        if (_state.value.connectivity == Connectivity.Disconnected) return
        if (_state.value.manualMode) return
        val take = takeManualUC ?: return
        viewModelScope.launch {
            val paired = currentDevice.value ?: return@launch
            // Allocate under the session mutex, send outside it: an
            // in-flight loop send never blocks this immediate frame's
            // allocation (and vice versa).
            val prepared = sessionMutex.withLock { sessionCounterLocked(paired) }
            if (prepared == null) {
                // Exhausted era: surface the repair copy here too (the loop
                // tick would on its next pass); a missing PSK stays silent —
                // the loop owns that copy.
                if (toRobotCounter.isExhausted()) sessionExpired(paired)
                return@launch
            }
            val (psk, next) = prepared
            val result = take(paired, psk, paired.era, next)
            if (result is Outcome.Failure) {
                _state.update { it.copy(error = result.reason) }
            }
        }
    }

    /**
     * Exits MANUAL: one shared counter is allocated and the resulting
     * `MANUAL_RELEASE` frame goes out 2x byte-identical via
     * [releaseManualUC] (same counter — one allocation, like `DISCONNECT`'s
     * 2x goodbye). Fire-and-forget best-effort: the robot never replies and
     * every send error is already swallowed inside the use case, so the
     * result is ignored here; the telemetry MANUAL bit converging to clear
     * is the confirmation.
     *
     * Disconnected gate (same as [onTakeManual]): while
     * [RobotControlUiState.connectivity] is [Connectivity.Disconnected] this
     * is a no-op (the robot's own command-stale exit covers the stop).
     */
    fun onReleaseManual() {
        if (_state.value.connectivity == Connectivity.Disconnected) return
        val release = releaseManualUC ?: return
        viewModelScope.launch {
            val paired = currentDevice.value ?: return@launch
            val prepared = sessionMutex.withLock { sessionCounterLocked(paired) } ?: return@launch
            val (psk, next) = prepared
            release(paired, psk, paired.era, next)
        }
    }

    /**
     * Plays one catalog expression: one signed `EXPR_TRIGGER` frame goes
     * out immediately via [triggerExpressionUC] (sharing the session
     * counter with the loop tick under [sessionMutex]). Exactly one frame
     * per tap, never repeated — the robot gates on MANUAL and replies
     * nothing, so a robot-side drop is silent by protocol.
     *
     * Gated to the reported MANUAL hold (same language as the sticks: no
     * MANUAL report, no send) plus the [Connectivity.Disconnected] no-op
     * guard. Unknown ids are rejected silently client-side against the
     * fetched catalog (the picker only lists fetched entries, so this
     * fires only on a stale tap); a transport failure surfaces its reason
     * (the tap was an explicit user action). When no session is loaded yet
     * the frame is skipped silently except exhaustion, which surfaces the
     * repair copy like the immediate e-stop path.
     */
    fun onTriggerExpression(id: Int) {
        if (_state.value.connectivity == Connectivity.Disconnected) return
        if (!_state.value.manualMode) return
        val trigger = triggerExpressionUC ?: return
        val knownIds = _state.value.expressions.map { it.id }.toSet()
        if (id !in knownIds) return
        viewModelScope.launch {
            val paired = currentDevice.value ?: return@launch
            // Allocate under the session mutex, send outside it: an
            // in-flight loop send never blocks this immediate frame's
            // allocation (and vice versa).
            val prepared = sessionMutex.withLock { sessionCounterLocked(paired) }
            if (prepared == null) {
                // Exhausted era: surface the repair copy here too (the loop
                // tick would on its next pass); a missing PSK stays silent —
                // the loop owns that copy.
                if (toRobotCounter.isExhausted()) sessionExpired(paired)
                return@launch
            }
            val (psk, next) = prepared
            val result = trigger(paired, psk, paired.era, next, id, knownIds)
            if (result is Outcome.Failure) {
                _state.update { it.copy(error = result.reason) }
            }
        }
    }

    /**
     * Pause sending while the control screen is in the background (leaving
     * the room turns off the lights). The next send-loop tick skips
     * vault/counter/send and just delays; the session cache is kept, so
     * [onForeground] resumes with no vault re-read and gapless counters.
     * Telemetry collection and the timeout counter keep running.
     *
     * Best-effort watermark flush (Layer A): fire-and-forget
     * [ToRobotCounter.flush] so a later kill loses nothing (one extra Room
     * write acceptable). Errors are swallowed — background must never crash.
     *
     * Best-effort `DISCONNECT` goodbye (robot alive rule): when [aliveUC]
     * is present and a session is loaded, one shared counter is allocated
     * and the resulting frame goes out 2x byte-identical (same counter —
     * one allocation, like `UNPAIR`'s 3x in [ForgetDeviceUseCase]) at the
     * stored device endpoint — the firmware never replies. Every send error
     * is swallowed (a missing PSK or an exhausted counter skips the send);
     * cancellation is never swallowed.
     */
    fun onBackground() {
        backgroundPaused = true
        viewModelScope.launch {
            try {
                toRobotCounter.flush()
            } catch (_: Exception) {
                // Swallowed: backgrounding must never crash.
            }
            sendDisconnectBestEffort()
        }
    }

    /**
     * Best-effort signed `DISCONNECT` (see [onBackground]): null [aliveUC]
     * (tests/legacy graphs), no paired device, a null PSK, or a null
     * counter allocation all skip the send; every transmit error is
     * swallowed so backgrounding always completes. Cancellation is never
     * swallowed.
     */
    private suspend fun sendDisconnectBestEffort() {
        val alive = aliveUC ?: return
        val dev = currentDevice.value ?: return
        try {
            // One allocation under the session mutex (same order as the
            // loop tick: session-then-holder), sent outside it.
            val prepared = sessionMutex.withLock { sessionCounterLocked(dev) } ?: return
            val (psk, counter) = prepared
            repeat(MAX_DISCONNECT_SENDS) {
                try {
                    alive.sendDisconnect(dev, psk, dev.era, counter)
                } catch (e: CancellationException) {
                    throw e
                } catch (_: Exception) {
                    // UDP loss / dead socket: the robot's 5 s alive window
                    // expires on its own; the local pause already happened.
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {
            // Malformed vault hex and friends: pause regardless.
        }
    }

    /**
     * Resume sending after [onBackground]. Forces the `ALIVE` clock due so
     * the next send-loop tick knocks immediately (re-pinning the robot's
     * unicast peer without waiting a full [ALIVE_INTERVAL_MS]), and marks
     * the expression catalog due so the same tick refetches it (returning
     * to the screen heals a lost fetch or a stale picker).
     */
    fun onForeground() {
        backgroundPaused = false
        ticksSinceAlive = aliveTickEvery
        expressionsDue = true
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
     * NOT reset the shared [toRobotCounter] (its live era must survive the VM
     * so the next session keeps a strictly increasing sequence; resetting
     * to the stale Room watermark would look like a replay to the robot).
     *   No flush here either: `onCleared` cannot suspend, so the graceful
     *   watermark path is [onBackground] (callers background before destroy);
     *   a kill without background still falls back to the 128-cadence
     *   watermark.
     */
    override fun onCleared() {
        if (sessionMutex.tryLock()) {
            try {
                sessionPsk = null
                pskMissingFor = null
                forgetFiredFor = null
            } finally {
                sessionMutex.unlock()
            }
        }
        super.onCleared()
    }

    companion object {
        /** Fire-and-forget `DISCONNECT` transmissions (no reply ever comes back). */
        const val MAX_DISCONNECT_SENDS = 2
    }
}
