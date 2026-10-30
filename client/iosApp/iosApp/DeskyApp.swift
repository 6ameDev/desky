import SwiftUI
import shared

/// App-wide object graph (T8). Koin boots once via the Kotlin entry point
/// (`initKoinForIos`, see `Kotlin/IosKoin.kt`); each ViewModel is resolved
/// once here and the same instances are passed into every view's wrapper.
/// No business logic lives in Swift: views render snapshots and call intents.
@MainActor
final class SharedGraph: ObservableObject {
    let pairingVM: PairingViewModel
    let controlVM: RobotControlViewModel
    let settingsVM: SettingsViewModel
    let homeVM: HomeViewModel
    let drivingVM: DrivingViewModel
    let expressionsVM: ExpressionsViewModel

    init() {
        // Swift names come from SKIE/apinotes: SharedIosKoinKt ->
        // IosKoinKt, SharedKoin_coreKoin -> Koin_coreKoin.
        let koin = IosKoinKt.doInitKoinForIos()
        pairingVM = koin.pairingViewModel()
        controlVM = koin.robotControlViewModel()
        settingsVM = koin.settingsViewModel()
        homeVM = koin.homeViewModel()
        drivingVM = koin.drivingViewModel()
        expressionsVM = koin.expressionsViewModel()
        // App-scope Wi-Fi rejoin monitor (TASK 4.ii): started once here so
        // it outlives any single tab; views observe `.wifiRejoined` /
        // `isOnWifi` on the shared singleton. Monitor-only, no Kotlin API.
        WifiRejoinMonitor.shared.start()
    }
}

enum DeskyTab: Hashable {
    case pairing
    case home
    case driving
    case expressions
    case control
    case settings
}

/// View-layer replica of the shared `SessionGateViewModel` fence. Koin
/// exposes no typed accessor for that VM and Swift cannot mint the KClass
/// token its generic get requires, so this replays the same rule over the
/// pairing VM's state: the first emission only arms, later transitions to
/// nil latch dead and non-nil latch alive.
@MainActor
final class SessionGateState: ObservableObject {
    @Published private(set) var provenDead = false
    private let vm: PairingViewModel

    init(vm: PairingViewModel) {
        self.vm = vm
    }

    func collect() async {
        var armed = false
        var lastId: String?
        for await state in vm.state {
            let id = state.pairedDevice?.id
            guard armed else {
                armed = true
                lastId = id
                continue
            }
            if lastId == id { continue }
            lastId = id
            provenDead = (id == nil)
        }
    }
}

@main
struct DeskyApp: App {
    @StateObject private var graph: SharedGraph
    @StateObject private var gate: SessionGateState
    @State private var selection: DeskyTab = .pairing
    @Environment(\.scenePhase) private var scenePhase

    init() {
        let graph = SharedGraph()
        _graph = StateObject(wrappedValue: graph)
        _gate = StateObject(wrappedValue: SessionGateState(vm: graph.pairingVM))
    }

    var body: some Scene {
        WindowGroup {
            // TabView mirrors Android's nav graph (pairing <-> control <->
            // settings) with tabs instead of a back stack; auto-jumps below
            // reproduce the onPaired / onUnpaired navigations.
            TabView(selection: $selection) {
                PairingView(vm: graph.pairingVM, selection: $selection)
                    .tabItem { Label("Pair", systemImage: "antenna.radiowaves.left.and.right") }
                    .tag(DeskyTab.pairing)
                HomeView(vm: graph.homeVM, selection: $selection)
                    .tabItem { Label("Home", systemImage: "house") }
                    .tag(DeskyTab.home)
                DrivingView(vm: graph.drivingVM, selection: $selection)
                    .tabItem { Label("Drive", systemImage: "gamecontroller") }
                    .tag(DeskyTab.driving)
                ExpressionsView(vm: graph.expressionsVM, selection: $selection)
                    .tabItem { Label("Expressions", systemImage: "face.smiling") }
                    .tag(DeskyTab.expressions)
                ControlView(vm: graph.controlVM)
                    .tabItem { Label("Control", systemImage: "slider.horizontal.3") }
                    .tag(DeskyTab.control)
                SettingsView(vm: graph.settingsVM, selection: $selection)
                    .tabItem { Label("Settings", systemImage: "gear") }
                    .tag(DeskyTab.settings)
            }
            .task { await gate.collect() }
            // Global unpaired fence (mirrors DeskyNavHost's provenDead
            // navigation): a pairing that dies mid-session forces pairing.
            .onChange(of: gate.provenDead) { dead in
                if dead { selection = .pairing }
            }
            // App-background pause/resume: ControlView's onDisappear covers
            // tab leaves; this covers the home-button path while Controls
            // is visible (both funnel into the same VM flags).
            .onChange(of: scenePhase) { phase in
                switch phase {
                case .background:
                    graph.controlVM.onBackground()
                case .active:
                    graph.controlVM.onForeground()
                default:
                    break
                }
            }
        }
    }
}
