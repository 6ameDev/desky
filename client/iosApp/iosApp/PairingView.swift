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
}

struct PairingView: View {
    @StateObject private var state: PairingViewState
    @Binding var selection: DeskyTab

    init(vm: PairingViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: PairingViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Pair with Desky").font(.title2).bold()
            SecureField("Pairing code", text: Binding(
                get: { state.snapshot.pin },
                set: { state.onPinChange($0) }
            ))
            .textFieldStyle(.roundedBorder)
            .keyboardType(.numberPad)
            .autocorrectionDisabled(true)
            .textInputAutocapitalization(.never)
            Button(state.snapshot.isPairing ? "Pairing…" : "Pair") {
                state.onPair()
            }
            .buttonStyle(.borderedProminent)
            .disabled(state.snapshot.isPairing)
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
            if state.snapshot.pairedDevice != nil { selection = .control }
        }
        .onChange(of: state.snapshot.pairedDevice?.id) { newId in
            if newId != nil { selection = .control }
        }
    }
}
