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

private let speedStep = 0.1

private func snapSpeed(_ value: Double) -> Double {
    (round(value / speedStep) * speedStep).clamped(to: 0...1)
}

struct SettingsView: View {
    @StateObject private var state: SettingsViewState

    // Draft text for the PID/SSID editors. Initialized once from the first
    // snapshot and NOT resynced, so a save round-trip never clobbers
    // in-flight typing (Android re-remembers on every config change; the
    // intents sent are identical).
    @State private var pidP: String
    @State private var pidI: String
    @State private var pidD: String
    @State private var ssid: String

    @State private var showLedSheet = false
    @State private var showPidSheet = false
    @State private var showSsidSheet = false
    @State private var showForgetConfirm = false
    @State private var stagedLed = 0
    @State private var speedDraft: Double?
    @State private var pidError: String?
    @State private var ssidError: String?

    init(vm: SettingsViewModel) {
        _state = StateObject(wrappedValue: SettingsViewState(vm: vm))
        let config = vm.state.value.config
        _pidP = State(initialValue: String(config.pidP))
        _pidI = State(initialValue: String(config.pidI))
        _pidD = State(initialValue: String(config.pidD))
        _ssid = State(initialValue: config.wifiSsid)
    }

    var body: some View {
        let config = state.snapshot.config
        return ScrollView {
            VStack(alignment: .leading, spacing: DeskySpacing.s16) {
                Text("Settings").font(.title2).bold()
                // Fixed-height feedback slot: the spinner toggles inside a box
                // that is always present, so saving never shifts the list.
                if state.snapshot.isSaving {
                    ProgressView()
                        .frame(maxWidth: .infinity, minHeight: 22)
                } else {
                    Spacer()
                        .frame(maxWidth: .infinity, minHeight: 22)
                }
                SectionLabel("General")
                SettingsCard {
                    SettingTitle(
                        title: "Speed limit",
                        subtitle: state.snapshot.isSaving
                            ? "\(Int(config.speedLimit * 100))% · Saving…"
                            : "\(Int(config.speedLimit * 100))%"
                    )
                    Slider(value: Binding(
                        get: { speedDraft ?? Double(config.speedLimit) },
                        set: { speedDraft = snapSpeed($0) }
                    ), in: 0...1, step: speedStep, onEditingChanged: { editing in
                        if !editing {
                            if let draft = speedDraft { state.onSpeedLimit(Float(draft)) }
                            speedDraft = nil
                        }
                    })
                    SettingsDivider()
                    SettingRow(
                        title: "LED profile",
                        subtitle: "Profile \(config.ledProfile)"
                    ) {
                        stagedLed = Int(config.ledProfile)
                        showLedSheet = true
                    }
                    SettingsDivider()
                    SettingRow(
                        title: "PID gains",
                        subtitle: "P \(config.pidP) · I \(config.pidI) · D \(config.pidD)"
                    ) {
                        pidError = nil
                        showPidSheet = true
                    }
                }
                SectionLabel("Network")
                SettingsCard {
                    SettingRow(
                        title: "Wi-Fi network",
                        subtitle: config.wifiSsid.isEmpty ? "Not set" : config.wifiSsid
                    ) {
                        ssidError = nil
                        showSsidSheet = true
                    }
                }
                if let error = state.snapshot.error {
                    Text(error).foregroundColor(.deskyDanger)
                }
                SectionLabel("Device")
                SettingsCard {
                    Button("Forget device") { showForgetConfirm = true }
                        .font(.system(size: DeskyType.bSize))
                        .foregroundColor(.deskyDanger)
                        .frame(maxWidth: .infinity, minHeight: 48, alignment: .leading)
                        .accessibilityLabel("Forget device")
                        .confirmationDialog(
                            "Forget device?",
                            isPresented: $showForgetConfirm,
                            titleVisibility: .visible
                        ) {
                            Button("Forget device", role: .destructive) { state.onForgetDevice() }
                            Button("Cancel", role: .cancel) { }
                        } message: {
                            Text("This removes the paired device from this phone.")
                        }
                }
            }
            .padding(.horizontal, DeskySpacing.s16)
            .padding(.vertical, DeskySpacing.s24)
        }
        .background(screen)
        .task { await state.collect() }
        .sheet(isPresented: $showLedSheet) {
            LedSheet(
                current: Int(config.ledProfile),
                staged: $stagedLed,
                onPick: { state.onLedProfile(Int32($0)) },
                onDismiss: { showLedSheet = false }
            )
        }
        .sheet(isPresented: $showPidSheet) {
            EditorSheet(title: "PID gains", onApply: {
                if applyPid() { showPidSheet = false }
            }) {
                HStack(spacing: DeskySpacing.s8) {
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
                }
                if let pidError {
                    Text(pidError).foregroundColor(.deskyDanger)
                }
            }
        }
        .sheet(isPresented: $showSsidSheet) {
            EditorSheet(title: "Wi-Fi network", onApply: {
                let trimmed = ssid.trimmingCharacters(in: .whitespacesAndNewlines)
                if trimmed.isEmpty {
                    ssidError = "Enter a Wi-Fi name"
                } else {
                    ssidError = nil
                    state.onWifiSsid(trimmed)
                    showSsidSheet = false
                }
            }) {
                TextField("Wi-Fi SSID", text: Binding(
                    get: { ssid },
                    set: { ssid = String($0.prefix(32)) }
                ))
                    .textFieldStyle(.roundedBorder)
                    .autocorrectionDisabled(true)
                    .textInputAutocapitalization(.never)
                if let ssidError {
                    Text(ssidError).foregroundColor(.deskyDanger)
                }
            }
        }
    }

    private func applyPid() -> Bool {
        guard let p = Float(pidP), let i = Float(pidI), let d = Float(pidD) else {
            pidError = "Enter valid numbers for P, I and D"
            return false
        }
        pidError = nil
        state.onPid(p: p, i: i, d: d)
        return true
    }

    private var screen: Color {
        colorScheme == .dark ? Color.deskyCard : Color.deskyCanvas
    }

    @Environment(\.colorScheme) private var colorScheme
}

private struct SectionLabel: View {
    let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        Text(text)
            .font(.system(size: DeskyType.cSize, weight: .medium))
            .foregroundColor(.deskySecondary)
    }
}

private struct SettingsCard<Content: View>: View {
    @ViewBuilder let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: DeskySpacing.s8) {
            content
        }
        .padding(DeskySpacing.s16)
        .frame(maxWidth: 560, alignment: .leading)
        .background(Color.deskyCard)
        .cornerRadius(DeskyRadius.r28)
        .overlay(
            RoundedRectangle(cornerRadius: DeskyRadius.r28)
                .stroke(Color.deskyMetal, lineWidth: 1)
        )
    }
}

private struct SettingsDivider: View {
    var body: some View {
        Rectangle()
            .fill(Color.deskyMetal.opacity(0.5))
            .frame(height: 1)
    }
}

private struct SettingTitle: View {
    let title: String
    let subtitle: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title)
                .font(.system(size: DeskyType.bSize))
            Text(subtitle)
                .font(.system(size: DeskyType.subSize))
                .foregroundColor(.deskySecondary)
        }
    }
}

private struct SettingRow: View {
    let title: String
    let subtitle: String
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                    .font(.system(size: DeskyType.bSize))
                Text(subtitle)
                    .font(.system(size: DeskyType.subSize))
                    .foregroundColor(.deskySecondary)
            }
            .frame(maxWidth: .infinity, minHeight: 56, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundColor(.deskyText)
        .accessibilityLabel("\(title), \(subtitle)")
    }
}

private struct LedSheet: View {
    let current: Int
    @Binding var staged: Int
    let onPick: (Int) -> Void
    let onDismiss: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: DeskySpacing.s8) {
            Text("LED profile")
                .font(.system(size: DeskyType.bSize, weight: .semibold))
            ForEach(0...7, id: \.self) { index in
                Button {
                    staged = index
                } label: {
                    HStack(spacing: DeskySpacing.s8) {
                        Image(systemName: staged == index ? "largecircle.fill.circle" : "circle")
                            .foregroundColor(.deskyPrimary)
                        Text("\(index)")
                            .font(.system(size: DeskyType.bSize))
                    }
                    .frame(maxWidth: .infinity, minHeight: 48, alignment: .leading)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundColor(.deskyText)
                .accessibilityLabel("Profile \(index)")
            }
            Button("Done") {
                if staged != current { onPick(staged) }
                onDismiss()
            }
            .buttonStyle(.borderedProminent)
            .frame(maxWidth: .infinity, minHeight: 48)
        }
        .padding(DeskySpacing.s16)
    }
}

private struct EditorSheet<Content: View>: View {
    let title: String
    let onApply: () -> Void
    @ViewBuilder let content: Content

    init(title: String, onApply: @escaping () -> Void, @ViewBuilder content: () -> Content) {
        self.title = title
        self.onApply = onApply
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: DeskySpacing.s16) {
            Text(title)
                .font(.system(size: DeskyType.bSize, weight: .semibold))
            content
            Button("Apply", action: onApply)
                .buttonStyle(.borderedProminent)
                .frame(maxWidth: .infinity, minHeight: 48)
        }
        .padding(DeskySpacing.s16)
    }
}

private extension Double {
    func clamped(to range: ClosedRange<Double>) -> Double {
        min(max(self, range.lowerBound), range.upperBound)
    }
}

/// Keystroke guard mirroring Android's float-input regex: partial entries
/// like "-" or "1." stay typable, anything else never lands in the draft.
private func isValidFloatInput(_ value: String) -> Bool {
    value.isEmpty || value.range(of: "^-?\\d*\\.?\\d*$", options: .regularExpression) != nil
}
