import SwiftUI
import shared

/// App-wide object graph. Koin boots once via the Kotlin entry point
/// (`initKoinForIos`, see `Kotlin/IosKoin.kt`); each ViewModel is resolved
/// once here and the same instances are passed into every view's wrapper.
/// No business logic lives in Swift: views render snapshots and call intents.
@MainActor
final class SharedGraph: ObservableObject {
    let pairingVM: PairingViewModel
    let settingsVM: SettingsViewModel
    let homeVM: HomeViewModel
    let connectionManager: ConnectionManager
    let driveController: DriveController
    let drivingVM: DrivingViewModel
    let expressionsVM: ExpressionsViewModel

    init() {
        // Swift names come from SKIE/apinotes: SharedIosKoinKt ->
        // IosKoinKt, SharedKoin_coreKoin -> Koin_coreKoin.
        let koin = IosKoinKt.doInitKoinForIos()
        pairingVM = koin.pairingViewModel()
        settingsVM = koin.settingsViewModel()
        homeVM = koin.homeViewModel()
        connectionManager = koin.connectionManager()
        driveController = koin.driveController()
        drivingVM = koin.drivingViewModel()
        expressionsVM = koin.expressionsViewModel()
        // App-scope Wi-Fi rejoin monitor: started once here so
        // it outlives any single tab; views observe `.wifiRejoined` /
        // `isOnWifi` on the shared singleton. Monitor-only, no Kotlin API.
        WifiRejoinMonitor.shared.start()
    }
}

/// Unpaired gate: true while no device is paired. The root shows the pairing
/// flow full-screen then, and the two-tab bar only once paired.
@MainActor
final class SessionGateState: ObservableObject {
    @Published private(set) var unpaired = true
    private let vm: PairingViewModel

    init(vm: PairingViewModel) {
        self.vm = vm
    }

    func collect() async {
        for await state in vm.state {
            unpaired = state.pairedDevice == nil
        }
    }
}

/// Runtime orientation gate. The plist declares every iPhone orientation so
/// the driving screen can rotate; this mask keeps everything else portrait.
/// Driving unlocks on appear and re-locks on disappear. Plain shared state
/// (no isolation) so the delegate can read it synchronously.
/// All access happens on the main thread: SwiftUI appear handlers write it,
/// the delegate reads it back on the main thread.
final class OrientationLock: @unchecked Sendable {
    static let shared = OrientationLock()
    private let lock = NSLock()
    private var gated: UIInterfaceOrientationMask = .portrait

    var mask: UIInterfaceOrientationMask {
        get { lock.withLock { gated } }
        set { lock.withLock { gated = newValue } }
    }
}

final class OrientationDelegate: NSObject, UIApplicationDelegate {
    func application(
        _ application: UIApplication,
        supportedInterfaceOrientationsFor window: UIWindow?
    ) -> UIInterfaceOrientationMask {
        OrientationLock.shared.mask
    }
}

@main
struct DeskyApp: App {
    @UIApplicationDelegateAdaptor(OrientationDelegate.self) private var orientationDelegate
    @StateObject private var graph: SharedGraph
    @StateObject private var gate: SessionGateState
    @State private var selection: DeskyTab = .home
    @Environment(\.scenePhase) private var scenePhase

    init() {
        let graph = SharedGraph()
        _graph = StateObject(wrappedValue: graph)
        _gate = StateObject(wrappedValue: SessionGateState(vm: graph.pairingVM))
    }

    var body: some Scene {
        WindowGroup {
            Group {
                if gate.unpaired {
                    PairingView(vm: graph.pairingVM, selection: $selection)
                } else {
                    // Exactly the two functional tabs. Drive and
                    // Expressions live in the Home tab's stack, pushed
                    // from its tiles — never tabs themselves.
                    TabView(selection: $selection) {
                        HomeView(
                            homeVM: graph.homeVM,
                            drivingVM: graph.drivingVM,
                            expressionsVM: graph.expressionsVM
                        )
                        .tabItem { Label("Home", systemImage: "house") }
                        .tag(DeskyTab.home)
                        SettingsView(vm: graph.settingsVM)
                            .tabItem { Label("Settings", systemImage: "gear") }
                            .tag(DeskyTab.settings)
                    }
                    .tint(Color.deskyPrimary)
                }
            }
            .task { await gate.collect() }
            .onChange(of: scenePhase) { phase in
                switch phase {
                case .background:
                    graph.connectionManager.onBackground()
                    graph.driveController.onBackground()
                case .active:
                    graph.connectionManager.onForeground()
                    graph.driveController.onForeground()
                default:
                    break
                }
            }
        }
    }
}
