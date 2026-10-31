import SwiftUI
import shared

/// Passive wrapper around the shared `HomeViewModel`.
@MainActor
final class HomeViewState: ObservableObject {
    @Published private(set) var snapshot: HomeUiState
    private let vm: HomeViewModel

    init(vm: HomeViewModel) {
        self.vm = vm
        snapshot = vm.state.value
    }

    func collect() async {
        for await state in vm.state {
            snapshot = state
        }
    }
}

struct HomeView: View {
    @StateObject private var state: HomeViewState
    @Binding var selection: DeskyTab
    @Environment(\.colorScheme) private var colorScheme

    private let ink = Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0)
    private let subInk = Color(red: 0x6F/255.0, green: 0x6A/255.0, blue: 0x5E/255.0)

    init(vm: HomeViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: HomeViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        let snapshot = state.snapshot
        let linkReady = snapshot.aliveness != Aliveness.unpaired
        let eyesOpen = snapshot.aliveness == Aliveness.connected
        let screen = colorScheme == .dark ? Color.deskyCard : Color.deskyCanvas
        GeometryReader { geo in
            ScrollView {
                VStack(spacing: 16) {
                    Spacer()
                    BotHead(eyesOpen: eyesOpen)
                    PillsRow(pills: snapshot.pills)
                    Spacer()
                        .frame(height: 8)
                    HStack(spacing: 16) {
                        Spacer()
                        ActionTile(
                            title: "Drive",
                            subtitle: linkReady ? "Manual control" : "Offline",
                            enabled: linkReady
                        ) { selection = .driving }
                        ActionTile(
                            title: "Expressions",
                            subtitle: linkReady ? "Play moods" : "Offline",
                            enabled: linkReady
                        ) { selection = .expressions }
                        Spacer()
                    }
                    Spacer()
                    Button("Settings") { selection = .settings }
                        .buttonStyle(.plain)
                        .foregroundColor(Color.deskyText)
                        .frame(minHeight: 48)
                }
                .frame(minHeight: geo.size.height - 40)
            }
            .scrollIndicators(.hidden)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 20)
        .background(screen)
        .task { await state.collect() }
    }
}

private struct BotHead: View {
    let eyesOpen: Bool

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 36)
                .fill(Color(red: 0xC8/255.0, green: 0xCA/255.0, blue: 0xD2/255.0))
                .frame(width: 180, height: 158)
            RoundedRectangle(cornerRadius: 29)
                .fill(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                .frame(width: 144, height: 126)
            HStack(spacing: 13) {
                RoundedRectangle(cornerRadius: 11)
                    .fill(Color(red: 0x46/255.0, green: 0xD8/255.0, blue: 0xFF/255.0))
                    .frame(width: 36, height: eyesOpen ? 38 : 8)
                RoundedRectangle(cornerRadius: 11)
                    .fill(Color(red: 0x46/255.0, green: 0xD8/255.0, blue: 0xFF/255.0))
                    .frame(width: 36, height: eyesOpen ? 38 : 8)
            }
        }
        .accessibilityHidden(true)
    }
}

private struct PillsRow: View {
    let pills: [HomePill]

    var body: some View {
        HStack(spacing: 8) {
            Spacer()
            ForEach(pills, id: \.label) { pill in
                Text(pill.value)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundColor(Color.deskyCard)
                    .lineLimit(1)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                    .background(Color.deskySecondary)
                    .cornerRadius(20)
                    .accessibilityLabel("\(pill.label): \(pill.value)")
            }
            Spacer()
        }
    }
}

private struct ActionTile: View {
    let title: String
    let subtitle: String
    let enabled: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            VStack(spacing: 4) {
                Text(title)
                    .font(.system(size: 15, weight: .bold))
                Text(subtitle)
                    .font(.system(size: 12))
                    .foregroundColor(Color(red: 0x6F/255.0, green: 0x6A/255.0, blue: 0x5E/255.0))
                    .lineLimit(2, reservesSpace: false)
                    .multilineTextAlignment(.center)
            }
            .frame(width: 106, height: 104)
        }
        .buttonStyle(.plain)
        .background(.white.opacity(enabled ? 1 : 0.6))
        .foregroundColor(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
        .cornerRadius(16)
        .disabled(!enabled)
    }
}
