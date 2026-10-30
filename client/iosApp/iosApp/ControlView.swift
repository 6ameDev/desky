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
    func onTakeManual() { vm.onTakeManual() }
    func onReleaseManual() { vm.onReleaseManual() }
    func onTriggerExpression(id: Int32) { vm.onTriggerExpression(id: id) }
    // Rejoin knock target: forces the ALIVE clock due (idempotent
    // due-flag, same path Android's `networkAvailable` flow takes).
    func onForeground() { vm.onForeground() }
    func onBackground() { vm.onBackground() }
}

struct ControlView: View {
    @StateObject private var state: ControlViewState
    @ObservedObject private var wifi = WifiRejoinMonitor.shared

    init(vm: RobotControlViewModel) {
        _state = StateObject(wrappedValue: ControlViewState(vm: vm))
    }

    var body: some View {
        let snapshot = state.snapshot
        let controlsEnabled = snapshot.connectivity != Connectivity.disconnected
        let manualControls = controlsEnabled && snapshot.manualMode
        return VStack(spacing: 12) {
            Text("Controls").font(.title2).bold()
            if !wifi.isOnWifi {
                Text("Not on Wi-Fi — join the desky Wi-Fi in Settings to drive.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            TelemetryBar(snapshot: snapshot)
            if snapshot.manualMode {
                Button("Release Manual") { state.onReleaseManual() }
                    .buttonStyle(.bordered)
                    .disabled(!manualControls)
            } else {
                Button("Take Manual") { state.onTakeManual() }
                    .buttonStyle(.borderedProminent)
                    .disabled(!controlsEnabled)
            }
            ExpressionPicker(
                expressions: snapshot.expressions,
                enabled: manualControls
            ) { state.onTriggerExpression(id: $0) }
            JoystickPad(enabled: manualControls) { throttle, steering in
                state.onStick(throttle: throttle, steering: steering)
            }
            if snapshot.eStopped {
                Button("Release E-STOP") { state.onReleaseEmergency() }
                    .buttonStyle(.bordered)
                    .disabled(!controlsEnabled)
            } else {
                Button("E-STOP") { state.onEmergencyStop() }
                    .buttonStyle(.borderedProminent)
                    .tint(.red)
                    .disabled(!controlsEnabled)
            }
            if let error = snapshot.error {
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
        // Mirror Android's ON_PAUSE: leaving the tab pauses the send loop
        // (flush + goodbye); returning re-knocks via onAppear above.
        .onDisappear { state.onBackground() }
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
            Text(snapshot.manualMode ? "Manual: active" : "Manual: inactive")
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

/// Expression picker: tap selects, Apply sends one trigger while MANUAL
/// is reported (same gating language as the sticks).
struct ExpressionPicker: View {
    let expressions: [ExpressionEntry]
    let enabled: Bool
    var onTrigger: (Int32) -> Void
    @State private var selectedId: Int32?

    var body: some View {
        VStack(spacing: 4) {
            Text("Expressions")
            if !expressions.isEmpty {
                let selected = selectedId ?? expressions[0].id
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(expressions, id: \.id) { entry in
                            if entry.id == selected {
                                Button(entry.name) { selectedId = entry.id }
                                    .buttonStyle(.borderedProminent)
                                    .disabled(!enabled)
                            } else {
                                Button(entry.name) { selectedId = entry.id }
                                    .buttonStyle(.bordered)
                                    .disabled(!enabled)
                            }
                        }
                    }
                }
                Button("Apply") { onTrigger(selected) }
                    .buttonStyle(.borderedProminent)
                    .disabled(!enabled)
            }
        }
    }
}

/// Drag joystick: normalized stick deflection in -1…1, recentered on release.
/// Up-drag is positive throttle (matches Android's `-y / radius`).
/// Disabled gate: gestures move nothing and send nothing.
struct JoystickPad: View {
    var enabled: Bool = true
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
        .opacity(enabled ? 1 : 0.4)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    guard enabled else { return }
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
                    if enabled { onStick(0, 0) }
                }
        )
    }
}
