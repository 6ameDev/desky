package com.desky.shared.domain

import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow

/**
 * Never-emitting default for [UdpSocketBinder.networkAvailable]: legacy
 * platform impls that do not override the property (and the JVM/iOS null
 * binder path) collect zero events, so rejoin stays inert there and the
 * 2 Hz `ALIVE` cadence remains the only rejoin path.
 */
private val NoopNetworkAvailable = MutableSharedFlow<Unit>(extraBufferCapacity = 1)

/**
 * Platform hook that pins the UDP socket to a specific network, attached
 * post-construction by [com.desky.shared.di.dataModule] when non-null.
 *
 * - Android passes a Wi-Fi binder (the phone may also have mobile data,
 *   whose default route cannot reach the LAN robot — see `WifiSocketBinder`
 *   KDoc). It emits
 *   [networkAvailable] from `onAvailable` (1 s debounce) so a network
 *   switch forces an immediate `ALIVE` like boot-ready / foreground.
 * - JVM/iOS pass null (no binding; the POSIX/JVM socket uses the system
 *   default route). GAP (documented, no-op): iOS has no `NWPathMonitor`
 *   binder yet, so rejoin there relies on the steady 2 Hz `ALIVE` cadence
 *   until a binder lands — no code change needed on that call site.
 *
 * Kept as an interface (rather than an Android type in the
 * `dataModule` signature) so common code compiles for every target and the
 * iOS call site needs no changes. [networkAvailable] has a default
 * no-op implementation so existing platform impls do not break: only the
 * Android binder overrides it.
 */
interface UdpSocketBinder {
    /** Register for the platform network and pin [manager]'s socket to it. */
    fun attach(manager: UdpSocketManager)

    /**
     * Release the platform network registration. Idempotent; safe to call
     * from [UdpSocketManager.close].
     */
    fun detach()

    /**
     * Hot rejoin signal: emits once per platform network-available event
     * (Android `onAvailable`, debounced). Non-suspending
     * (`MutableSharedFlow(extraBufferCapacity = 1)` + `tryEmit`, same idiom
     * as the socket `incoming` flows) because system callbacks cannot
     * suspend. The control ViewModel collects this and forces the `ALIVE`
     * clock due — a network switch is treated like boot-ready / foreground:
     * immediate `ALIVE`, then the steady 2 Hz resumes.
     */
    val networkAvailable: SharedFlow<Unit>
        get() = NoopNetworkAvailable
}
