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

private enum HomeDestination: Hashable {
    case driving
    case expressions
}

struct HomeView: View {
    @StateObject private var state: HomeViewState
    @State private var path = NavigationPath()
    private let drivingVM: DrivingViewModel
    private let expressionsVM: ExpressionsViewModel

    init(
        homeVM: HomeViewModel,
        drivingVM: DrivingViewModel,
        expressionsVM: ExpressionsViewModel
    ) {
        _state = StateObject(wrappedValue: HomeViewState(vm: homeVM))
        self.drivingVM = drivingVM
        self.expressionsVM = expressionsVM
    }

    var body: some View {
        NavigationStack(path: $path) {
            GeometryReader { geo in
                ScrollView {
                    VStack(spacing: 16) {
                        Spacer()
                        BotHead(eyesOpen: state.snapshot.aliveness == Aliveness.connected)
                        PillsRow(pills: state.snapshot.pills)
                        Spacer()
                            .frame(height: 8)
                        // Fluid tiles: full size on wide phones, shrinking with
                        // tighter gaps as the viewport narrows so three tiles
                        // never clip or scroll.
                        let fullTile = (geo.size.width - 32 - 16 * 2) / 3
                        let gap: CGFloat = fullTile >= 106 ? 16 : 8
                        let tile = max(84, min(106, (geo.size.width - 32 - gap * 2) / 3))
                        HStack(spacing: gap) {
                            Spacer()
                            DeskyTile(
                                title: "Drive",
                                subtitle: linkReady ? "Manual control" : "Offline",
                                enabled: linkReady,
                                onTap: { path.append(HomeDestination.driving) },
                                size: tile
                            )
                            DeskyTile(
                                title: "Expressions",
                                subtitle: linkReady ? "Play moods" : "Offline",
                                enabled: linkReady,
                                onTap: { path.append(HomeDestination.expressions) },
                                size: tile
                            )
                            DeskyTile(
                                title: "Camera",
                                subtitle: "Take Snaps",
                                enabled: true,
                                onTap: { print("Camera tile tapped") },
                                size: tile
                            )
                            Spacer()
                        }
                        Spacer()
                    }
                    .frame(minHeight: geo.size.height - 40)
                }
                .scrollIndicators(.hidden)
            }
            .padding(.horizontal, 16)
            .padding(.vertical, 20)
            .background(screen)
            .task { await state.collect() }
            .navigationDestination(for: HomeDestination.self) { destination in
                switch destination {
                case .driving:
                    DrivingView(vm: drivingVM) { popPath() }
                case .expressions:
                    ExpressionsView(vm: expressionsVM) { popPath() }
                }
            }
        }
    }

    private var linkReady: Bool {
        state.snapshot.aliveness != Aliveness.unpaired
    }

    private var screen: Color {
        Color.deskyCanvas
    }

    private func popPath() {
        if !path.isEmpty {
            path.removeLast()
        }
    }
}

private struct BotHead: View {
    let eyesOpen: Bool

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            ZStack {
                RoundedRectangle(cornerRadius: w * 0.20)
                    .fill(Color(red: 0xC8/255.0, green: 0xCA/255.0, blue: 0xD2/255.0))
                    .frame(width: w, height: w * 158 / 180)
                RoundedRectangle(cornerRadius: w * 0.16)
                    .fill(Color(red: 0x1E/255.0, green: 0x1B/255.0, blue: 0x16/255.0))
                    .frame(width: w * 0.80, height: w * 126 / 180)
                HStack(spacing: w * 13 / 180) {
                    RoundedRectangle(cornerRadius: w * 11 / 180)
                        .fill(Color(red: 0x46/255.0, green: 0xD8/255.0, blue: 0xFF/255.0))
                        .frame(width: w * 0.20, height: eyesOpen ? w * 38 / 180 : 8)
                    RoundedRectangle(cornerRadius: w * 11 / 180)
                        .fill(Color(red: 0x46/255.0, green: 0xD8/255.0, blue: 0xFF/255.0))
                        .frame(width: w * 0.20, height: eyesOpen ? w * 38 / 180 : 8)
                }
            }
            .frame(width: w, height: w * 158 / 180)
        }
        .aspectRatio(180 / 158, contentMode: .fit)
        .padding(.horizontal, DeskySpacing.s24)
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
