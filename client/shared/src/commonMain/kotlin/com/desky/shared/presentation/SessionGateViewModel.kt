package com.desky.shared.presentation

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.desky.shared.domain.DeviceRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch

/**
 * Global unpaired fence: `true` once a previously-loaded pairing is proven dead.
 *
 * Cold-start rule: the FIRST [DeviceRepository.observePaired] emission only
 * arms the gate and never navigates. A cold-start `null` must NOT navigate —
 * `startDestination=PAIRING` already shows pairing, so there is nowhere to
 * force. A cold-start non-null likewise stays `false` (already paired).
 *
 * Fence semantics: after arming, every transition to `null` latches `true`
 * (force to Pairing) and every non-null latches `false` (paired again).
 * Keyed on [Boolean] and `distinctUntilChanged` upstream, so duplicate repo
 * emissions never re-fire navigation. State-driven ([StateFlow]), not
 * channel-driven: late collectors (recreated Activities) still see the fence.
 *
 * Broken rows (e.g. vault secret missing while the Room row survives)
 * converge here via the repo emitting `null` — [ForgetDeviceUseCase] clears
 * the row — never via this VM reading the vault itself. VMs never hold a
 * NavController; navigation observes [provenDead].
 */
class SessionGateViewModel(
    deviceRepo: DeviceRepository,
) : ViewModel() {
    private val _provenDead = MutableStateFlow(false)
    val provenDead: StateFlow<Boolean> = _provenDead.asStateFlow()

    init {
        viewModelScope.launch {
            var loaded = false
            deviceRepo.observePaired().distinctUntilChanged().collect { paired ->
                if (!loaded) {
                    loaded = true
                    // First emission only arms, never navigates: if null stay
                    // false (startDestination=PAIRING already shows pairing);
                    // if non-null false.
                    _provenDead.value = false
                    return@collect
                }
                _provenDead.value = (paired == null)
            }
        }
    }
}
