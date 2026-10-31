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

private enum PairingMode {
    case method
    case code
}

struct PairingView: View {
    @StateObject private var state: PairingViewState
    @Binding var selection: DeskyTab
    @State private var showScan = false
    @State private var mode: PairingMode = .method
    // Hint-only: PairingViewModel owns no ALIVE loop, so the rejoin knock
    // has no target here — it fires from the scenePhase foreground path
    // via ConnectionManager; this view only reflects `isOnWifi`.
    @ObservedObject private var wifi = WifiRejoinMonitor.shared

    private func pairingLabel(_ snapshot: PairingUiState) -> String {
        guard snapshot.isPairing else { return "Connect" }
        return snapshot.pairingAttempt <= 1
            ? "Pairing…"
            : "Pairing… (attempt \(snapshot.pairingAttempt))"
    }

    private func pairingStep(_ snapshot: PairingUiState) -> Int {
        snapshot.pairingAttempt <= 1 ? 1 : 2
    }

    init(vm: PairingViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: PairingViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        GeometryReader { geo in
            ScrollView {
                VStack(spacing: 20) {
            if state.snapshot.isPairing {
                StepperDots(step: pairingStep(state.snapshot))
            }
            if !wifi.isOnWifi {
                Text("Not on Wi-Fi — join the desky Wi-Fi in Settings to pair.")
                    .font(.footnote)
                    .foregroundColor(.white.opacity(0.85))
            }
            Spacer()
            if state.snapshot.pairedDevice != nil {
                if let device = state.snapshot.pairedDevice {
                    Text("Paired: \(device.alias) (\(device.ip):\(device.port))")
                        .foregroundColor(.white)
                    Button("Unpair") { state.onUnpair() }
                        .buttonStyle(.plain)
                        .padding(.vertical, 12)
                        .padding(.horizontal, 16)
                        .background(.white)
                        .foregroundColor(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                        .cornerRadius(18)
                }
            } else {
                switch mode {
                case .method:
                    BotShuttle()
                    Spacer()
                    QrTileCard { showScan = true }
                    Button("enter code instead") { mode = .code }
                        .buttonStyle(.plain)
                        .foregroundColor(.white)
                        .font(.system(size: 17, weight: .bold))
                case .code:
                    Text("Enter code")
                        .font(.system(size: DeskyType.hSize, weight: .bold))
                        .foregroundColor(.white)
                    CodeEntryField(
                        pin: state.snapshot.pin,
                        onPinChange: { state.onPinChange($0) }
                    )
                    Button(action: { state.onPair() }) {
                        Text(pairingLabel(state.snapshot))
                            .font(.system(size: 17, weight: .bold))
                            .frame(width: 136, height: 49)
                    }
                    .buttonStyle(.plain)
                    .background(.white)
                    .foregroundColor(Color.deskyPrimary)
                    .cornerRadius(28)
                    .disabled(state.snapshot.isPairing || state.snapshot.pairedDevice != nil)
                    Button("scan instead") { mode = .method }
                        .buttonStyle(.plain)
                        .foregroundColor(.white)
                        .font(.system(size: 17, weight: .bold))
                }
            }
            if let error = state.snapshot.error {
                Text(error)
                    .foregroundColor(Color(red: 0xD9/255.0, green: 0x30/255.0, blue: 0x25/255.0))
                    .padding(.vertical, 12)
                    .padding(.horizontal, 16)
                    .background(.white)
                    .cornerRadius(18)
            }
            Spacer()
            if mode == .method {
                Spacer()
            }
                }
                .frame(minHeight: geo.size.height - 40)
            }
            .scrollIndicators(.hidden)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 20)
        .background(Color.deskyPrimary)
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

private struct BotShuttle: View {
    @State private var phase: CGFloat = 0
    private let w: CGFloat = 60
    private let h: CGFloat = 40

    var body: some View {
        GeometryReader { geo in
            let amplitude = max((geo.size.width - w) / 2 - 8, 24)
            let offsetX = phase < 1
                ? -amplitude + 2 * amplitude * phase
                : amplitude - 2 * amplitude * (phase - 1)
            HStack(spacing: 0) {
                Spacer()
                VStack(spacing: 0) {
                    HStack(spacing: 0) {
                        Spacer()
                        RoundedRectangle(cornerRadius: 2)
                            .fill(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                            .frame(width: 12, height: 15)
                            .overlay(
                                RoundedRectangle(cornerRadius: 1.5)
                                    .fill(Color(red: 0x46/255.0, green: 0xD8/255.0, blue: 0xFF/255.0))
                                    .frame(width: 4, height: 8)
                                    .offset(x: 2.5),
                                alignment: .center
                            )
                    }
                    .frame(width: w, height: 15)
                    RoundedRectangle(cornerRadius: 4)
                        .fill(.white)
                        .frame(width: 54, height: 18)
                    RoundedRectangle(cornerRadius: 2)
                        .fill(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                        .frame(width: 58, height: 7)
                }
                .frame(width: w, height: h)
                .offset(x: offsetX)
                .scaleEffect(x: phase < 1 ? 1 : -1, y: 1)
                Spacer()
            }
        }
        .frame(height: 40)
        .accessibilityHidden(true)
        .onAppear {
            withAnimation(.linear(duration: 8).repeatForever(autoreverses: false)) {
                phase = 2
            }
        }
    }
}

private struct QrTileCard: View {
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            ZStack {
                RoundedRectangle(cornerRadius: 12)
                    .fill(.black)
                    .frame(width: 130, height: 130)
                VStack(spacing: 0) {
                    HStack(spacing: 0) {
                        FinderSquare()
                        Spacer()
                        FinderSquare()
                    }
                    Spacer()
                    HStack(spacing: 0) {
                        FinderSquare()
                        Spacer()
                        FinderSquare()
                    }
                }
                .frame(width: 110, height: 110)
                Text("scan me")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundColor(.white)
            }
            .frame(width: 170, height: 170)
        }
        .buttonStyle(.plain)
        .background(.white)
        .foregroundColor(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
        .cornerRadius(18)
    }
}

private struct FinderSquare: View {
    var body: some View {
        RoundedRectangle(cornerRadius: 4)
            .fill(.white)
            .frame(width: 24, height: 24)
    }
}

private struct CodeEntryField: View {
    let pin: String
    let onPinChange: (String) -> Void
    @FocusState private var focused: Bool

    private var digits: String {
        String(pin.filter(\.isNumber).prefix(6))
    }

    var body: some View {
        ZStack {
            TextField("", text: Binding(
                get: { digits },
                set: { onPinChange(String($0.filter(\.isNumber).prefix(6))) }
            ))
            .keyboardType(.numberPad)
            .autocorrectionDisabled(true)
            .textInputAutocapitalization(.never)
            .focused($focused)
            .opacity(0.01)
            .frame(width: 1, height: 1)
            HStack(spacing: 8) {
                ForEach(0..<6, id: \.self) { index in
                    ZStack {
                        RoundedRectangle(cornerRadius: 12)
                            .fill(.white)
                            .frame(width: 48, height: 56)
                        if index < digits.count {
                            Text(String(digits[digits.index(digits.startIndex, offsetBy: index)]))
                                .font(.system(size: 24, weight: .bold))
                                .foregroundColor(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                        } else if index == min(digits.count, 5) && focused {
                            RoundedRectangle(cornerRadius: 1)
                                .fill(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                                .frame(width: 2, height: 28)
                        }
                    }
                }
            }
            .contentShape(Rectangle())
            .onTapGesture { focused = true }
        }
        .frame(width: 328, height: 56)
        .onAppear { focused = true }
    }
}

private struct StepperDots: View {
    let step: Int

    var body: some View {
        HStack(spacing: 8) {
            StepDot(active: step == 1)
            StepDot(active: step == 2)
        }
    }
}

private struct StepDot: View {
    let active: Bool

    var body: some View {
        Circle()
            .fill(active ? .white : .white.opacity(0.35))
            .frame(width: 8, height: 8)
    }
}
