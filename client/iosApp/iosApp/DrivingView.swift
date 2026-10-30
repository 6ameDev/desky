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
    func onEnter() { vm.onEnter() }
    func requestExit() { vm.requestExit() }
    func dismissExit() { vm.dismissExit() }
    func confirmExit() { vm.confirmExit() }
}

struct DrivingView: View {
    @StateObject private var state: DrivingViewState
    @Binding var selection: DeskyTab

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
            JoystickPad(enabled: snapshot.aliveness == Aliveness.connected) { throttle, steering in
                state.onStick(throttle: throttle, steering: steering)
            }
            if let telemetry = snapshot.telemetry {
                Text(telemetry.driving ? "Driving" : "Idle")
            }
            Spacer()
        }
        .padding()
        .onAppear { state.onEnter() }
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
