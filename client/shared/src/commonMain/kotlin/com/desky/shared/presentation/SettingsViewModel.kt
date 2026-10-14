package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.ObserveDeviceConfigUseCase
import com.desky.shared.domain.Outcome
import com.desky.shared.domain.RobotConfig
import com.desky.shared.domain.SaveDeviceConfigUseCase
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/** Settings-screen state (UDF: single [StateFlow] + explicit intents). */
data class SettingsUiState(
    val config: RobotConfig = RobotConfig(),
    val isSaving: Boolean = false,
    val unpaired: Boolean = false,
    val error: String? = null,
)

/**
 * Drives the settings screen: mirrors the stored [RobotConfig] on init, and
 * each edit updates the state synchronously and persists it via [saveUC].
 *
 * Save guard: state is always updated, but persistence is serialized through
 * a [Mutex] with [SettingsUiState.isSaving] held for the whole in-flight
 * window — rapid edits queue instead of racing in the repository, and no
 * user input is ever dropped for a busy flag.
 */
class SettingsViewModel(
    private val saveUC: SaveDeviceConfigUseCase,
    private val observeUC: ObserveDeviceConfigUseCase,
    private val forgetUC: ForgetDeviceUseCase,
) : ViewModel() {

    private val _state = MutableStateFlow(SettingsUiState())
    val state: StateFlow<SettingsUiState> = _state.asStateFlow()

    private val saveMutex = Mutex()

    init {
        viewModelScope.launch {
            observeUC().collect { config ->
                _state.update { it.copy(config = config) }
            }
        }
    }

    fun onSpeedLimit(value: Float) = updateAndPersist { it.copy(speedLimit = value) }

    fun onLedProfile(value: Int) = updateAndPersist { it.copy(ledProfile = value) }

    fun onPid(p: Float, i: Float, d: Float) = updateAndPersist { it.copy(pidP = p, pidI = i, pidD = d) }

    fun onWifiSsid(value: String) = updateAndPersist { it.copy(wifiSsid = value) }

    private fun updateAndPersist(transform: (RobotConfig) -> RobotConfig) {
        val next = try {
            transform(_state.value.config)
        } catch (e: IllegalArgumentException) {
            // Model validation (e.g. speedLimit range) maps to error state,
            // never to a crash out of a UI intent.
            _state.update { it.copy(error = e.message) }
            return
        }
        _state.update { it.copy(config = next, error = null) }
        viewModelScope.launch {
            _state.update { it.copy(isSaving = true) }
            try {
                saveMutex.withLock {
                    when (val result = saveUC(next)) {
                        is Outcome.Success -> Unit
                        is Outcome.Failure ->
                            _state.update { it.copy(error = result.reason) }
                    }
                }
            } finally {
                _state.update { it.copy(isSaving = false) }
            }
        }
    }

    fun onForgetDevice() {
        viewModelScope.launch {
            when (val result = forgetUC()) {
                is Outcome.Success ->
                    _state.update { it.copy(unpaired = true, error = null) }
                is Outcome.Failure ->
                    _state.update { it.copy(error = result.reason) }
            }
        }
    }
}
