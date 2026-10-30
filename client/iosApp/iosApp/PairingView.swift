import SwiftUI
import shared

/// Passive wrapper around the shared `PairingViewModel`: publishes each
/// `PairingUiState` emission for SwiftUI and forwards intents verbatim.
@MainActor
final class PairingViewState: ObservableObject {
    @Published private(set) var snapshot: PairingUiState
    private let vm: PairingViewModel

    init(vm: PairingViewModel) {
        self.vm = vm
        snapshot = vm.state.value
    }

    func collect() async {
        for await state in vm.state {
            snapshot = state
        }
    }

    func onPinChange(_ pin: String) { vm.onPinChange(pin: pin) }
    func onPair() { vm.onPair() }
    func onUnpair() { vm.onUnpair() }
    func onQrScanned(_ raw: String) { vm.onQrScanned(raw: raw) }
}

struct PairingView: View {
    @StateObject private var state: PairingViewState
    @Binding var selection: DeskyTab
    @State private var showScan = false
    // Hint-only: PairingViewModel owns no ALIVE loop, so the rejoin knock
    // has no target here — the knock fires from ControlView (plus its
    // onAppear covers pairing -> controls navigation).
    @ObservedObject private var wifi = WifiRejoinMonitor.shared

    private func pairingLabel(_ snapshot: PairingUiState) -> String {
        guard snapshot.isPairing else { return "Pair" }
        return snapshot.pairingAttempt <= 1
            ? "Pairing…"
            : "Pairing… (attempt \(snapshot.pairingAttempt))"
    }

    init(vm: PairingViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: PairingViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Pair with Desky").font(.title2).bold()
            if !wifi.isOnWifi {
                Text("Not on Wi-Fi — join the desky Wi-Fi in Settings to pair.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            TextField("Pairing code", text: Binding(
                get: { state.snapshot.pin },
                set: { state.onPinChange($0) }
            ))
            .textFieldStyle(.roundedBorder)
            .keyboardType(.numberPad)
            .autocorrectionDisabled(true)
            .textInputAutocapitalization(.never)
            Button(pairingLabel(state.snapshot)) {
                state.onPair()
            }
            .buttonStyle(.borderedProminent)
            .disabled(state.snapshot.isPairing || state.snapshot.pairedDevice != nil)
            // QR shortcut next to the manual field (manual entry stays
            // forever): decoded digits feed the identical onPair path.
            Button("Scan QR") { showScan = true }
                .buttonStyle(.bordered)
                .disabled(state.snapshot.pairedDevice != nil)
            if let error = state.snapshot.error {
                Text(error).foregroundColor(.red)
            }
            if let device = state.snapshot.pairedDevice {
                Text("Paired: \(device.alias) (\(device.ip):\(device.port))")
                HStack {
                    Button("Open Controls") { selection = .control }
                        .buttonStyle(.borderedProminent)
                    Button("Unpair") { state.onUnpair() }
                        .buttonStyle(.bordered)
                }
            }
            Spacer()
        }
        .padding()
        .task { await state.collect() }
        // Mirror Android's onPaired navigation (fires on launch too when a
        // pairing is already stored).
        .onAppear {
            if state.snapshot.pairedDevice != nil { selection = .home }
        }
        .onChange(of: state.snapshot.pairedDevice?.id) { newId in
            if newId != nil { selection = .home }
        }
        .sheet(isPresented: $showScan) {
            ScanQrView(state: state)
        }
    }
}
