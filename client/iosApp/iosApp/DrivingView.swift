import SwiftUI
import shared

/// Passive wrapper around the shared `DrivingViewModel`.
@MainActor
final class DrivingViewState: ObservableObject {
    @Published private(set) var snapshot: DrivingUiState
    private let vm: DrivingViewModel

    init(vm: DrivingViewModel) {
        self.vm = vm
        snapshot = vm.state.value
    }

    func collect() async {
        for await state in vm.state {
            snapshot = state
        }
    }

    func onStick(throttle: Float, steering: Float) {
        vm.onStick(throttle: throttle, steering: steering)
    }
    func onEmergencyStop() { vm.onEmergencyStop() }
    func onReleaseEmergency() { vm.onReleaseEmergency() }
    func activate() { vm.activate() }
    func deactivate() { vm.deactivate() }
    func requestExit() { vm.requestExit() }
    func dismissExit() { vm.dismissExit() }
    func confirmExit() { vm.confirmExit() }
}

struct DrivingView: View {
    @StateObject private var state: DrivingViewState
    @Binding var selection: DeskyTab
    @Environment(\.scenePhase) private var scenePhase
    @State private var visible = false

    init(vm: DrivingViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: DrivingViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        let snapshot = state.snapshot
        return VStack(spacing: 12) {
            Text("Driving").font(.title2).bold()
            Text("Link: \(snapshot.aliveness.name)")
            if let telemetry = snapshot.telemetry {
                Text("Distance: \(telemetry.distanceMm) mm")
            } else {
                Text("Waiting for telemetry…")
            }
            JoystickPad(enabled: snapshot.aliveness == Aliveness.connected && !snapshot.eStopped) { throttle, steering in
                state.onStick(throttle: throttle, steering: steering)
            }
            if snapshot.eStopped {
                Button("STOPPED — tap to release") { state.onReleaseEmergency() }
                    .tint(.red)
            } else {
                Button("EMERGENCY STOP") { state.onEmergencyStop() }
                    .disabled(snapshot.aliveness != Aliveness.connected)
            }
            if let telemetry = snapshot.telemetry {
                Text(telemetry.driving ? "Driving" : "Idle")
            }
            Spacer()
        }
        .padding()
        .onAppear { visible = true; state.activate() }
        .onDisappear { visible = false; state.deactivate() }
        // Home-button path: onAppear does not refire on foreground return,
        // so re-take only while this tab is still the visible one.
        .onChange(of: scenePhase) { phase in
            switch phase {
            case .background:
                state.deactivate()
            case .active:
                if visible { state.activate() }
            default:
                break
            }
        }
        .task { await state.collect() }
        .alert(
            "Leave driving?",
            isPresented: Binding(
                get: { state.snapshot.pendingExit },
                set: { leaving in if !leaving { state.dismissExit() } }
            )
        ) {
            Button("Leave", role: .destructive) {
                state.confirmExit()
                selection = .home
            }
            Button("Stay", role: .cancel) { state.dismissExit() }
        } message: {
            Text("The robot will be released and stop.")
        }
    }
}
