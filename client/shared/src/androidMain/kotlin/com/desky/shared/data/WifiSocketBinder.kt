package com.desky.shared.data

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import com.desky.shared.domain.UdpSocketBinder
import com.desky.shared.domain.UdpSocketManager

/**
 * Platform [UdpSocketBinder] for Android, constructed in `DeskyApp` from the
 * application context and threaded through `dataModule(...)` like
 * `createDeskyDatabase` / `createBondSecretStore`.
 */
fun createWifiSocketBinder(context: Context): UdpSocketBinder = WifiSocketBinder(context)

/**
 * The `NetworkRequest` spec for the robot link, exposed (rather than inlined
 * in [WifiSocketBinder.attach]) so the decision is auditable in one place.
 *
 * `TRANSPORT_WIFI` with the `NET_CAPABILITY_INTERNET` requirement REMOVED:
 * the stock `NetworkRequest` default requires INTERNET, which the robot AP
 * (`desky`, no upstream) never provides — so a default request never matches
 * and the socket stays on the default route (mobile data when active, and
 * pairing packets never reach the robot). Root-cause fix for pairing loss
 * with mobile data on.
 *
 * JVM/commonTest note: `android.net.*` does not exist on the JVM, so nothing
 * in this file runs under `:shared:jvmTest` (androidMain is not even compiled
 * there). There is no unit-testable pure logic here by design — the binder is
 * a thin shell over `ConnectivityManager`; verify on-device (pair with mobile
 * data active) and via `:androidApp:assembleDebug`.
 */
internal fun wifiNetworkRequest(): NetworkRequest = NetworkRequest.Builder()
    .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
    .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    .build()

/**
 * Binds the single UDP socket owned by [DatagramSocketManager] to the Wi-Fi
 * network via per-socket [Network.bindSocket] (never process-wide binding).
 * Needs `ACCESS_NETWORK_STATE` + `CHANGE_NETWORK_STATE` (both install-time):
 * `requestNetwork()` itself demands the latter — without it the call throws
 * `SecurityException` (crashed app startup on first install; see manifest).
 *
 * Failure policy: binding must NEVER break the app. [attach] swallows any
 * exception from `requestNetwork()` (missing permission, no ConnectivityManager)
 * and leaves the socket on the default route — pairing/drive then behave
 * exactly as before this binder existed. Same for per-callback re-binds.
 *
 * Lifecycle: [attach] calls `requestNetwork()` and registers the callback;
 * `onAvailable` re-binds the socket on EVERY call (the fd is re-pointed at
 * the new network, so no socket recreate is needed and the ephemeral port
 * stays fixed — the robot replies to the source port either way); `onLost`
 * marks [wifiStale] and the next `onAvailable` re-binds it. Sends issued before
 * the first `onAvailable` still use the default route — the pairing/drive
 * retry loops cover that window. [detach] unregisters the callback and is
 * idempotent (also called from `DatagramSocketManager.close()`; safe when
 * never attached — the `attached` flag short-circuits under lock).
 *
 * Ownership: app scope (`DeskyApp` Koin singleton), NOT any ViewModel —
 * see `DatagramSocketManager` KDoc. Holds the application context only
 * (never an activity), so [attach] from the app-lifetime Koin graph cannot
 * leak a UI context.
 */
internal class WifiSocketBinder(context: Context) : UdpSocketBinder {
    private val connectivityManager =
        context.applicationContext.getSystemService(ConnectivityManager::class.java)

    private val lock = Any()

    private var manager: DatagramSocketManager? = null
    private var attached = false

    /** Last Wi-Fi network the socket was bound to; null until the first `onAvailable`. */
    @Volatile
    var lastNetwork: Network? = null
        private set

    /**
     * True until the first `onAvailable` binds the socket, and again after
     * `onLost` drops that network. Diagnostic only — the socket fd persists
     * across loss (rebind-only, no recreate) and traffic resumes on the next
     * `onAvailable`.
     */
    @Volatile
    var wifiStale = true
        private set

    private val callback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            lastNetwork = network
            // Re-bind on every call: re-points the fd at the current network.
            // Never throws out of a system callback, and wifiStale tracks the
            // Boolean result (a swallowed inner failure used to lie): failure
            // leaves the socket on its previous route (same as binder-absent
            // behavior).
            val bound = try {
                synchronized(lock) { manager }?.bindToWifiNetwork(network) ?: false
            } catch (_: Exception) {
                false
            }
            wifiStale = !bound
        }

        override fun onLost(network: Network) {
            if (network == lastNetwork) wifiStale = true
            // Documented limitation (rebind-only): the socket keeps its fd
            // and port; the next onAvailable re-binds it. No recreate, so no
            // ephemeral-port churn for the robot to track.
        }
    }

    override fun attach(manager: UdpSocketManager) {
        val androidManager = manager as? DatagramSocketManager ?: return
        synchronized(lock) {
            if (attached) return
            attached = true
            this.manager = androidManager
            androidManager.setWifiBinder(this)
        }
        // Registered outside the lock: the callback re-enters via the lock.
        // Must never throw: this runs inside Koin singleton creation on the
        // app's startup path, where an exception kills the whole app. On any
        // failure (missing permission, no service) the socket simply stays on
        // the default route.
        try {
            connectivityManager?.requestNetwork(wifiNetworkRequest(), callback)
        } catch (_: Exception) {
            wifiStale = true
        }
    }

    override fun detach() {
        synchronized(lock) {
            if (!attached) return
            attached = false
            manager = null
        }
        try {
            connectivityManager?.unregisterNetworkCallback(callback)
        } catch (_: Exception) {
            // Never registered (attach short-circuited) — nothing to release.
        }
    }
}
