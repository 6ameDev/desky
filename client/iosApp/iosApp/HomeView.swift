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

    init(vm: HomeViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: HomeViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        let snapshot = state.snapshot
        VStack(alignment: .leading, spacing: 12) {
            Text("Desky").font(.title2).bold()
            if let device = snapshot.pairedDevice {
                Text("Paired: \(device.alias)")
            }
            Text("Link: \(snapshot.aliveness.name)")
            let linkReady = snapshot.aliveness != Aliveness.unpaired
            Button("Drive") { selection = .driving }
                .buttonStyle(.borderedProminent)
                .disabled(!linkReady)
            Button("Expressions") { selection = .expressions }
                .buttonStyle(.borderedProminent)
                .disabled(!linkReady)
            Button("Settings") { selection = .settings }
                .buttonStyle(.bordered)
            Spacer()
        }
        .padding()
        .task { await state.collect() }
    }
}
