package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.PairDeviceWithCodeUseCase
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * Pairing screen state (unidirectional data flow: single [StateFlow] +
 * explicit intents below).
 *
 * The robot endpoint is fixed (`192.168.4.1:3333` — pairing/auth v1), so
 * there is no IP/host field: [pin] holds the 6-digit pairing code shown on
 * the robot display.
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
     */
    fun onPair() {
        if (_state.value.isPairing) return
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
}
