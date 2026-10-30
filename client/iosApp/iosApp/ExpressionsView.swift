import SwiftUI
import shared

/// Passive wrapper around the shared `ExpressionsViewModel`.
@MainActor
final class ExpressionsViewState: ObservableObject {
    @Published private(set) var snapshot: ExpressionsUiState
    private let vm: ExpressionsViewModel

    init(vm: ExpressionsViewModel) {
        self.vm = vm
        snapshot = vm.state.value
    }

    func collect() async {
        for await state in vm.state {
            snapshot = state
        }
    }

    func onTriggerExpression(id: Int32) { vm.onTriggerExpression(id: id) }
    func activate() { vm.activate() }
    func deactivate() { vm.deactivate() }
    func requestExit() { vm.requestExit() }
    func dismissExit() { vm.dismissExit() }
    func confirmExit() { vm.confirmExit() }
}

struct ExpressionsView: View {
    @StateObject private var state: ExpressionsViewState
    @Binding var selection: DeskyTab
    @Environment(\.scenePhase) private var scenePhase
    @State private var visible = false

    init(vm: ExpressionsViewModel, selection: Binding<DeskyTab>) {
        _state = StateObject(wrappedValue: ExpressionsViewState(vm: vm))
        _selection = selection
    }

    var body: some View {
        let snapshot = state.snapshot
        let canTrigger = snapshot.aliveness == Aliveness.connected && snapshot.telemetry?.manual == true
        return VStack(spacing: 12) {
            Text("Expressions").font(.title2).bold()
            Text("Link: \(snapshot.aliveness.name)")
            ExpressionPicker(
                expressions: snapshot.expressions,
                enabled: canTrigger
            ) { state.onTriggerExpression(id: $0) }
            if let error = snapshot.error {
                Text(error).foregroundColor(.red)
            }
            Spacer()
        }
        .padding()
        .onAppear { visible = true; state.activate() }
        .onDisappear { visible = false; state.deactivate() }
        // Home-button path: onAppear does not refire on foreground return,
        // so re-take only while this tab is still the visible one.
        .onChange(of: scenePhase) { phase in
            switch phase {
            case .background:
                state.deactivate()
            case .active:
                if visible { state.activate() }
            default:
                break
            }
        }
        .task { await state.collect() }
        .alert(
            "Leave expressions?",
            isPresented: Binding(
                get: { state.snapshot.pendingExit },
                set: { leaving in if !leaving { state.dismissExit() } }
            )
        ) {
            Button("Leave", role: .destructive) {
                state.confirmExit()
                selection = .home
            }
            Button("Stay", role: .cancel) { state.dismissExit() }
        } message: {
            Text("Unsent expressions will be discarded.")
        }
    }
}
