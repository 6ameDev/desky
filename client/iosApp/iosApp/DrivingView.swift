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
    private let onExit: () -> Void
    @Environment(\.scenePhase) private var scenePhase
    @State private var visible = false

    init(vm: DrivingViewModel, onExit: @escaping () -> Void) {
        _state = StateObject(wrappedValue: DrivingViewState(vm: vm))
        self.onExit = onExit
    }

    var body: some View {
        let snapshot = state.snapshot
        return VStack {
            StatusCard(snapshot: snapshot)
                .frame(maxWidth: 480)
                .frame(maxWidth: .infinity, alignment: .top)
            Spacer()
            HStack(alignment: .center) {
                EstopButton(snapshot: snapshot, onEmergencyStop: { state.onEmergencyStop() }, onRelease: { state.onReleaseEmergency() })
                Spacer()
                JoystickPad(enabled: snapshot.aliveness == Aliveness.connected && !snapshot.eStopped) { throttle, steering in
                    state.onStick(throttle: throttle, steering: steering)
                }
            }
            .padding(.leading, DeskySpacing.s8)
            .padding(.bottom, DeskySpacing.s8)
        }
        .padding(.horizontal, DeskySpacing.s16)
        .padding(.vertical, DeskySpacing.s8)
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                Button("Close") { state.requestExit() }
            }
        }
        .toolbar(.hidden, for: .tabBar)
        .onAppear { visible = true; state.activate(); OrientationLock.shared.mask = .landscape }
        .onDisappear { visible = false; state.deactivate(); OrientationLock.shared.mask = .portrait }
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
                onExit()
            }
            Button("Stay", role: .cancel) { state.dismissExit() }
        } message: {
            Text("The robot will be released and stop.")
        }
    }
}

private struct StatusCard: View {
    let snapshot: DrivingUiState

    var body: some View {
        VStack(alignment: .leading, spacing: DeskySpacing.s8) {
            if let sessionError = snapshot.error {
                Text(sessionError).foregroundColor(.deskyDanger)
            }
            if let telemetry = snapshot.telemetry {
                HStack {
                    Text(snapshot.aliveness == Aliveness.connected ? "Online" : "Offline")
                    Spacer()
                    Text("\(telemetry.distanceMm) mm")
                    Spacer()
                    Text(telemetry.driving ? "Driving" : "Idle")
                }
            } else {
                Text("Waiting for telemetry…")
            }
        }
        .padding(.horizontal, DeskySpacing.s16)
        .padding(.vertical, 12)
        .frame(maxWidth: 560, alignment: .leading)
        .background(Color.deskyCard)
        .cornerRadius(DeskyRadius.r16)
        .overlay(
            RoundedRectangle(cornerRadius: DeskyRadius.r16)
                .stroke(Color.deskyMetal, lineWidth: 1)
        )
    }
}

private struct EstopButton: View {
    let snapshot: DrivingUiState
    let onEmergencyStop: () -> Void
    let onRelease: () -> Void

    var body: some View {
        VStack(spacing: 4) {
            Button(action: { snapshot.eStopped ? onRelease() : onEmergencyStop() }) {
                Text("E")
                    .font(.system(size: 28, weight: .bold))
                    .foregroundColor(.deskyCard)
                    .frame(width: 76, height: 76)
                    .background(Color.deskyDanger)
                    .clipShape(Circle())
            }
            .buttonStyle(.plain)
            .disabled(!snapshot.eStopped && snapshot.aliveness != Aliveness.connected)
            .accessibilityLabel(snapshot.eStopped ? "Emergency stop engaged, tap to release" : "Emergency stop")
            if snapshot.eStopped && snapshot.aliveness != Aliveness.connected {
                Text("Reconnect to release")
                    .font(.system(size: DeskyType.subSize))
                    .foregroundColor(.deskySecondary)
            }
        }
    }
}
