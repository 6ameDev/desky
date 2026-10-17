import SwiftUI
import shared

/// Passive wrapper around the shared `RobotControlViewModel`.
@MainActor
final class ControlViewState: ObservableObject {
    @Published private(set) var snapshot: RobotControlUiState
    private let vm: RobotControlViewModel

    init(vm: RobotControlViewModel) {
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
    // Rejoin knock target: forces the ALIVE clock due (idempotent
    // due-flag, same path Android's `networkAvailable` flow takes).
    func onForeground() { vm.onForeground() }
}

struct ControlView: View {
    @StateObject private var state: ControlViewState
    @ObservedObject private var wifi = WifiRejoinMonitor.shared

    init(vm: RobotControlViewModel) {
        _state = StateObject(wrappedValue: ControlViewState(vm: vm))
    }

    var body: some View {
        VStack(spacing: 12) {
            Text("Controls").font(.title2).bold()
            if !wifi.isOnWifi {
                Text("Not on Wi-Fi — join the desky Wi-Fi in Settings to drive.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            TelemetryBar(snapshot: state.snapshot)
            JoystickPad { throttle, steering in
                state.onStick(throttle: throttle, steering: steering)
            }
            if state.snapshot.eStopped {
                Button("Release E-STOP") { state.onReleaseEmergency() }
                    .buttonStyle(.bordered)
            } else {
                Button("E-STOP") { state.onEmergencyStop() }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
            }
            if let error = state.snapshot.error {
                Text(error).foregroundColor(.red)
            }
            Spacer()
        }
        .padding()
        .task { await state.collect() }
        // Mirror Android's ON_RESUME knock: entering the tab forces ALIVE
        // due so a rejoin that happened while on another tab still
        // re-pins immediately. `.wifiRejoined` covers rejoins while this
        // tab is visible; both funnel into the same idempotent flag.
        .onAppear { state.onForeground() }
        .onReceive(NotificationCenter.default.publisher(for: .wifiRejoined)) { _ in
            state.onForeground()
        }
    }
}

private struct TelemetryBar: View {
    let snapshot: RobotControlUiState

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Link: \(snapshot.connectivity.name)  Mode: \(snapshot.mode.name)")
            if let telemetry = snapshot.telemetry {
                Text("Distance: \(telemetry.distanceMm) mm")
                Text(String(format: "Pitch: %.1f°  Roll: %.1f°", telemetry.pitchDeg, telemetry.rollDeg))
                HStack(spacing: 8) {
                    Text(telemetry.cliff ? "CLIFF!" : "Cliff: clear")
                    Text(telemetry.driving ? "Driving" : "Idle")
                }
            } else {
                Text("Waiting for telemetry…")
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// Drag joystick: normalized stick deflection in -1…1, recentered on release.
/// Up-drag is positive throttle (matches Android's `-y / radius`).
private struct JoystickPad: View {
    var onStick: (_ throttle: Float, _ steering: Float) -> Void
    @State private var knob = CGSize.zero
    private let radius: CGFloat = 80

    var body: some View {
        ZStack {
            Circle()
                .fill(Color.secondary.opacity(0.2))
                .frame(width: radius * 2, height: radius * 2)
            Circle()
                .fill(Color.accentColor)
                .frame(width: 60, height: 60)
                .offset(knob)
        }
        .frame(width: radius * 2, height: radius * 2)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    var dx = value.translation.width
                    var dy = value.translation.height
                    let dist = sqrt(dx * dx + dy * dy)
                    if dist > radius {
                        let scale = radius / dist
                        dx *= scale
                        dy *= scale
                    }
                    knob = CGSize(width: dx, height: dy)
                    onStick(Float(-dy / radius), Float(dx / radius))
                }
                .onEnded { _ in
                    knob = .zero
                    onStick(0, 0)
                }
        )
    }
}
