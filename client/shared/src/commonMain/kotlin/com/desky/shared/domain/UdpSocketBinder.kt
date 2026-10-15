package com.desky.shared.domain

/**
 * Platform hook that pins the UDP socket to a specific network, attached
 * post-construction by [com.desky.shared.di.dataModule] when non-null.
 *
 * - Android passes a Wi-Fi binder (the robot AP has no internet, so the
 *   default route sends pairing/drive datagrams out over mobile data and
 *   they never reach the robot — see `WifiSocketBinder` KDoc).
 * - JVM/iOS pass null (no binding; the POSIX/JVM socket uses the system
 *   default route).
 *
 * Kept as a two-method interface (rather than an Android type in the
 * `dataModule` signature) so common code compiles for every target and the
 * iOS call site needs no changes.
 */
interface UdpSocketBinder {
    /** Register for the platform network and pin [manager]'s socket to it. */
    fun attach(manager: UdpSocketManager)

    /**
     * Release the platform network registration. Idempotent; safe to call
     * from [UdpSocketManager.close].
     */
    fun detach()
}
