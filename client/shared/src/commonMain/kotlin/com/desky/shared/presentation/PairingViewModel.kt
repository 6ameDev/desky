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
 */
data class PairingUiState(
    val ip: String = "",
    val pin: String = "",
    val isPairing: Boolean = false,
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

    fun onIpChange(ip: String) {
        _state.update { it.copy(ip = ip, error = null) }
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
        _state.update { it.copy(isPairing = true, error = null) }
        viewModelScope.launch {
            when (val result = pairUC(_state.value.ip, _state.value.pin)) {
                is Outcome.Success ->
                    _state.update { it.copy(isPairing = false, pairedDevice = result.data) }
                is Outcome.Failure ->
                    _state.update { it.copy(isPairing = false, error = result.reason) }
            }
        }
    }

    fun onUnpair() {
        viewModelScope.launch {
            when (val result = forgetUC()) {
                is Outcome.Success ->
                    _state.update { it.copy(pairedDevice = null, error = null) }
                is Outcome.Failure ->
                    _state.update { it.copy(error = result.reason) }
            }
        }
    }
}
