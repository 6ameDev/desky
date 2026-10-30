package com.desky.shared.features.control

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.ExpressionEntry
import com.desky.shared.core.Outcome
import com.desky.shared.core.PICKER_EXPRESSIONS
import com.desky.shared.core.STATIC_EXPRESSION_IDS
import com.desky.shared.core.Telemetry
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.TakeManualUseCase
import com.desky.shared.core.TriggerExpressionUseCase
import com.desky.shared.core.hexToBytes
import com.desky.shared.features.pairing.GetPairPskUseCase
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Expressions-screen state (UDF: single [StateFlow] + explicit intents below). */
data class ExpressionsUiState(
    val expressions: List<ExpressionEntry> = PICKER_EXPRESSIONS,
    val telemetry: Telemetry? = null,
    val aliveness: Aliveness = Aliveness.Unpaired,
    val lastTriggeredId: Int? = null,
    val error: String? = null,
    /**
     * Exit handshake: [requestExit] raises it (shell shows the confirm
     * dialog), [confirmExit] clears it (shell navigates away), [dismissExit]
     * clears it (shell stays).
     */
    val pendingExit: Boolean = false,
)

/**
 * Expressions screen: plays one catalog expression per tap via
 * [TriggerExpressionUseCase], gated on a Connected link with the MANUAL
 * hold reported. Entry via [onEnter] takes MANUAL first so triggers work
 * without driving first. Collects [ConnectionManager] aliveness plus
 * [TelemetryHub] telemetry; owns no loops. Exit is explicit via
 * [requestExit] / [confirmExit] / [dismissExit].
 */
class ExpressionsViewModel(
    private val deviceRepo: DeviceRepository,
    private val getPairPsk: GetPairPskUseCase,
    private val toRobotCounter: ToRobotCounter,
    private val triggerUC: TriggerExpressionUseCase?,
    private val takeManualUC: TakeManualUseCase? = null,
    connectionManager: ConnectionManager,
    telemetryHub: TelemetryHub,
) : ViewModel() {

    private val _state = MutableStateFlow(ExpressionsUiState())
    val state: StateFlow<ExpressionsUiState> = _state.asStateFlow()

    private val sessionMutex = Mutex()
    private var sessionPsk: ByteArray? = null
    private var pskMissingFor: String? = null
    private var entered = false

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
    }

    /**
     * Screen entry: one `MANUAL_TAKE` so triggers work without driving
     * first. Idempotent until exit; silent when unpaired, keyless, or
     * counter-exhausted.
     */
    fun onEnter() {
        if (entered) return
        entered = true
        val take = takeManualUC ?: return
        viewModelScope.launch {
            val device = deviceRepo.observePaired().first() ?: return@launch
            val prepared = sessionMutex.withLock {
                val psk = loadSessionPskLocked(device.id) ?: return@withLock null
                toRobotCounter.ensure(device.id, device.lastCounter, device.persistEvery, device.era)
                val counter = toRobotCounter.next() ?: return@withLock null
                Triple(device, psk, counter)
            } ?: return@launch
            take(prepared.first, prepared.second, prepared.first.era, prepared.third)
        }
    }

    /**
     * Plays one catalog expression (one `EXPR_TRIGGER`, never repeated).
     * Silent no-op while the link is not Connected, while MANUAL is not
     * reported, or for unknown ids; a transport failure surfaces its reason.
     */
    fun onTriggerExpression(id: Int) {
        val snapshot = _state.value
        if (snapshot.aliveness != Aliveness.Connected) return
        if (snapshot.telemetry?.manual != true) return
        if (id !in STATIC_EXPRESSION_IDS) return
        val trigger = triggerUC ?: return
        viewModelScope.launch {
            val device = deviceRepo.observePaired().first() ?: return@launch
            val prepared = sessionMutex.withLock {
                val psk = loadSessionPskLocked(device.id) ?: return@withLock null
                toRobotCounter.ensure(device.id, device.lastCounter, device.persistEvery, device.era)
                val counter = toRobotCounter.next() ?: return@withLock null
                psk to counter
            } ?: return@launch
            val (psk, counter) = prepared
            when (val result = trigger(device, psk, device.era, counter, id)) {
                is Outcome.Success ->
                    _state.update { it.copy(lastTriggeredId = id, error = null) }
                is Outcome.Failure ->
                    _state.update { it.copy(error = result.reason) }
            }
        }
    }

    fun requestExit() {
        _state.update { it.copy(pendingExit = true) }
    }

    fun dismissExit() {
        _state.update { it.copy(pendingExit = false) }
    }

    fun confirmExit() {
        entered = false
        _state.update { it.copy(pendingExit = false) }
    }

    private suspend fun loadSessionPskLocked(deviceId: String): ByteArray? {
        var psk = sessionPsk
        if (psk == null) {
            if (pskMissingFor == deviceId) return null
            val hex = getPairPsk() ?: run {
                pskMissingFor = deviceId
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
                pskMissingFor = deviceId
                return null
            }
            psk = decoded
            sessionPsk = psk
        }
        return psk
    }

    private companion object {
        const val PAIR_PSK_BYTE_LENGTH = 16
    }
}
