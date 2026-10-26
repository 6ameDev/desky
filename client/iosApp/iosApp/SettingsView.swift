import SwiftUI
import shared

/// Passive wrapper around the shared `SettingsViewModel`.
@MainActor
final class SettingsViewState: ObservableObject {
    @Published private(set) var snapshot: SettingsUiState
    private let vm: SettingsViewModel

    init(vm: SettingsViewModel) {
        self.vm = vm
        snapshot = vm.state.value
    }

    func collect() async {
        for await state in vm.state {
            snapshot = state
        }
    }

    func onSpeedLimit(_ value: Float) { vm.onSpeedLimit(value: value) }
    func onLedProfile(_ value: Int32) { vm.onLedProfile(value: value) }
    func onPid(p: Float, i: Float, d: Float) { vm.onPid(p: p, i: i, d: d) }
    func onWifiSsid(_ value: String) { vm.onWifiSsid(value: value) }
    func onForgetDevice() { vm.onForgetDevice() }
}

struct SettingsView: View {
    @StateObject private var state: SettingsViewState
    @Binding var selection: DeskyTab

    // Draft text for the PID/SSID editors, mirroring Android's local editor
    // state. Initialized once from the first snapshot and NOT resynced, so a
    // save round-trip never clobbers in-flight typing (Android re-remembers
    // on every config change; the intents sent are identical).
    @State private var pidP: String
    @State private var pidI: String
    @State private var pidD: String
    @State private var ssid: String

    init(vm: SettingsViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: SettingsViewState(vm: vm))
        _selection = selection
        let config = vm.state.value.config
        _pidP = State(initialValue: String(config.pidP))
        _pidI = State(initialValue: String(config.pidI))
        _pidD = State(initialValue: String(config.pidD))
        _ssid = State(initialValue: config.wifiSsid)
    }

    var body: some View {
        let config = state.snapshot.config
        return VStack(alignment: .leading, spacing: 12) {
            Text("Settings").font(.title2).bold()
            if state.snapshot.isSaving {
                ProgressView()
            }
            Text("Speed limit: \(Int(config.speedLimit * 100))%")
            Slider(value: Binding(
                get: { Double(config.speedLimit) },
                set: { state.onSpeedLimit(Float($0)) }
            ), in: 0...1)
            Stepper("LED profile: \(config.ledProfile)", value: Binding(
                get: { Int(config.ledProfile) },
                set: { state.onLedProfile(Int32($0)) }
            ), in: 0...7)
            HStack {
                TextField("P", text: Binding(
                    get: { pidP },
                    set: { if isValidFloatInput($0) { pidP = $0 } }
                ))
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.decimalPad)
                TextField("I", text: Binding(
                    get: { pidI },
                    set: { if isValidFloatInput($0) { pidI = $0 } }
                ))
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.decimalPad)
                TextField("D", text: Binding(
                    get: { pidD },
                    set: { if isValidFloatInput($0) { pidD = $0 } }
                ))
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.decimalPad)
                Button("Apply") { applyPid() }
                    .buttonStyle(.bordered)
            }
            HStack {
                TextField("Wi-Fi SSID", text: $ssid)
                    .textFieldStyle(.roundedBorder)
                    .autocorrectionDisabled(true)
                    .textInputAutocapitalization(.never)
                Button("Apply") { state.onWifiSsid(ssid) }
                    .buttonStyle(.bordered)
            }
            if let error = state.snapshot.error {
                Text(error).foregroundColor(.red)
            }
            if state.snapshot.isSaving {
                Text("Saving…")
            }
            Button("Forget device") { state.onForgetDevice() }
                .buttonStyle(.borderedProminent)
                .frame(maxWidth: .infinity)
            Spacer()
        }
        .padding()
        .task { await state.collect() }
        // Mirror Android's onUnpaired navigation back to pairing.
        .onChange(of: state.snapshot.unpaired) { unpaired in
            if unpaired { selection = .pairing }
        }
    }

    private func applyPid() {
        guard let p = Float(pidP), let i = Float(pidI), let d = Float(pidD) else { return }
        state.onPid(p: p, i: i, d: d)
    }
}

/// Keystroke guard mirroring Android's float-input regex: partial entries
/// like "-" or "1." stay typable, anything else never lands in the draft.
private func isValidFloatInput(_ value: String) -> Bool {
    value.isEmpty || value.range(of: "^-?\\d*\\.?\\d*$", options: .regularExpression) != nil
}
