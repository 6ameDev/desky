package com.desky.shared.features.pairing

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.Outcome
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.QrCodeParser
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Pairing screen state (unidirectional data flow: single [StateFlow] +
 * explicit intents below).
 *
 * The robot endpoint resolves via mDNS (`desky.local:3333` — pairing/auth
 * v1), so there is no IP/host field: [pin] holds the 6-digit pairing code
 * shown on the robot display.
 */
data class PairingUiState(
    val pin: String = "",
    val isPairing: Boolean = false,
    val pairingAttempt: Int = 1,
    val pairedDevice: PairedDevice? = null,
    val error: String? = null,
)

/**
 * Drives the pair/unpair screen.
 *
 * The current pairing is mirrored from [DeviceRepository.observePaired] on
 * init, so pairings performed elsewhere (or restored from Room) show up
 * without user action.
 */
class PairingViewModel(
    private val pairUC: PairDeviceWithCodeUseCase,
    private val forgetUC: ForgetDeviceUseCase,
    private val deviceRepo: DeviceRepository,
) : ViewModel() {

    private val _state = MutableStateFlow(PairingUiState())
    val state: StateFlow<PairingUiState> = _state.asStateFlow()

    init {
        viewModelScope.launch {
            deviceRepo.observePaired().collect { device ->
                _state.update { it.copy(pairedDevice = device) }
            }
        }
    }

    fun onPinChange(pin: String) {
        _state.update { it.copy(pin = pin, error = null) }
    }

    /**
     * Starts pairing. Guards re-entry via [PairingUiState.isPairing]: the flag
     * is set synchronously (before the coroutine is even scheduled), so a
     * second tap while a pairing is in flight is a no-op.
     *
     * Paired gate: while [PairingUiState.pairedDevice] is non-null the Pair
     * button is disabled (the UI binds `enabled` to this too) — only Forget
     * ([onUnpair], always enabled) plus the screen's auto-retry are offered,
     * so a second pair can never be stacked over a live one.
     */
    fun onPair() {
        if (_state.value.isPairing) return
        if (_state.value.pairedDevice != null) return
        // Snapshot the PIN before launch: the field stays enabled during
        // pairing (UX-owned), so reading inside the coroutine could race an edit.
        val code = _state.value.pin
        _state.update { it.copy(isPairing = true, pairingAttempt = 1, error = null) }
        viewModelScope.launch {
            when (val result = pairUC(code) { a, _ ->
                _state.update { it.copy(pairingAttempt = a) }
            }) {
                is Outcome.Success ->
                    _state.update { it.copy(isPairing = false, pairingAttempt = 1, pairedDevice = result.data) }
                is Outcome.Failure ->
                    _state.update { it.copy(isPairing = false, pairingAttempt = 1, error = result.reason) }
            }
        }
    }

    fun onUnpair() {
        viewModelScope.launch {
            when (val result = forgetUC()) {
                is Outcome.Success ->
                    _state.update { it.copy(pairedDevice = null, pairingAttempt = 1, error = null) }
                is Outcome.Failure ->
                    _state.update { it.copy(error = result.reason) }
            }
        }
    }

    /**
     * QR-scan shortcut feeding the identical [onPair] path as the manual
     * field (auto-pair on valid scan; the manual field stays as fallback
     * forever). The camera layer stops after the first valid decode and
     * ignores frames while [PairingUiState.isPairing]; this intent guards
     * the same window so a duplicate callback while a pairing is in flight
     * is a no-op. Invalid payloads reuse the [PairingUiState.error] field
     * (no new state).
     */
    fun onQrScanned(raw: String) {
        if (_state.value.isPairing) return
        val code = QrCodeParser.parse(raw)
        if (code == null) {
            _state.update { it.copy(error = "QR isn't a Desky code — try again") }
            return
        }
        _state.update { it.copy(pin = code, error = null) }
        onPair()
    }
}
