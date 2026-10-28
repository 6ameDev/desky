import Combine
import Foundation
import Network

extension Notification.Name {
    /// Posted (1 s debounced, like Android's `WifiSocketBinder`) whenever the
    /// Wi-Fi path becomes satisfied. Observers call the shared
    /// `RobotControlViewModel.onForeground()` — the same idempotent ALIVE
    /// due-flag Android's `networkAvailable` flow sets. Pure Swift:
    /// no Kotlin binder hookup / cinterop, no socket rebind (iOS stays on
    /// the raw BSD default route via `PosixUdpSocketManager`).
    static let wifiRejoined = Notification.Name("desky.wifiRejoined")
}

/// App-scope Wi-Fi reachability monitor (TASK 4.ii).
///
/// Started once from `SharedGraph.init()` so it outlives any single tab.
/// `NWPathMonitor(requiredInterfaceType: .wifi)` reports `.satisfied` only
/// while a Wi-Fi path is up, mirroring Android's Wi-Fi-only
/// `WifiSocketBinder` network request. Monitor-only: no
/// `NEHotspotConfiguration`, no entitlements, no Info.plist changes.
///
/// The monitor fires its handler immediately on start with the current
/// path, so app launch knocks once — same as Android's `onAvailable`
/// at register time, and harmless (`onForeground` is an idempotent
/// due-flag, not a transmit).
///
/// Simulator note: the API exists on the simulator but a wifi-required
/// monitor typically never reports `.satisfied` there, so the "not on
/// Wi-Fi" hint stays visible under sim — untested on simulator, verify
/// on device.
/// Concurrency: `@unchecked Sendable` is sound here by confinement, not by
/// locking — `lastFire` is touched only on the serial monitor `queue`,
/// `isOnWifi` only on main (`@Published` requirement), `started` once at
/// startup, the rest is immutable. Keep it that way if this class changes.
final class WifiRejoinMonitor: ObservableObject, @unchecked Sendable {
    static let shared = WifiRejoinMonitor()

    /// Matches `WifiSocketBinder.REJOIN_DEBOUNCE_MS`: flaps inside this
    /// window update `isOnWifi` but post no knock.
    private static let rejoinDebounce: TimeInterval = 1.0

    /// False when Wi-Fi is down (`status != .satisfied`) or expensive
    /// (hotspot tether). Drives the static helper hint on Pairing/Controls.
    /// Updated on the main queue (SwiftUI `@Published` requirement).
    @Published private(set) var isOnWifi = true

    private let monitor = NWPathMonitor(requiredInterfaceType: .wifi)
    private let queue = DispatchQueue(label: "desky.wifiRejoinMonitor")
    private var started = false
    /// Last knock fire. Touched only on `queue`, so no lock needed.
    private var lastFire = Date.distantPast

    func start() {
        guard !started else { return }
        started = true
        monitor.pathUpdateHandler = { [weak self] path in
            guard let self else { return }
            let onWifi = path.status == .satisfied && !path.isExpensive
            var knock = false
            if path.status == .satisfied,
               Date().timeIntervalSince(self.lastFire) >= Self.rejoinDebounce
            {
                self.lastFire = Date()
                knock = true
            }
            DispatchQueue.main.async {
                self.isOnWifi = onWifi
                if knock {
                    NotificationCenter.default.post(name: .wifiRejoined, object: nil)
                }
            }
        }
        monitor.start(queue: queue)
    }
}
