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

    init() {
        // Swift names come from SKIE/apinotes: SharedIosKoinKt ->
        // IosKoinKt, SharedKoin_coreKoin -> Koin_coreKoin.
        let koin = IosKoinKt.doInitKoinForIos()
        pairingVM = koin.pairingViewModel()
        controlVM = koin.robotControlViewModel()
        settingsVM = koin.settingsViewModel()
    }
}

enum DeskyTab: Hashable {
    case pairing
    case control
    case settings
}

@main
struct DeskyApp: App {
    @StateObject private var graph = SharedGraph()
    @State private var selection: DeskyTab = .pairing

    var body: some Scene {
        WindowGroup {
            // TabView mirrors Android's nav graph (pairing <-> control <->
            // settings) with tabs instead of a back stack; auto-jumps below
            // reproduce the onPaired / onUnpaired navigations.
            TabView(selection: $selection) {
                PairingView(vm: graph.pairingVM, selection: $selection)
                    .tabItem { Label("Pair", systemImage: "antenna.radiowaves.left.and.right") }
                    .tag(DeskyTab.pairing)
                ControlView(vm: graph.controlVM)
                    .tabItem { Label("Control", systemImage: "gamecontroller") }
                    .tag(DeskyTab.control)
                SettingsView(vm: graph.settingsVM, selection: $selection)
                    .tabItem { Label("Settings", systemImage: "gear") }
                    .tag(DeskyTab.settings)
            }
        }
    }
}
