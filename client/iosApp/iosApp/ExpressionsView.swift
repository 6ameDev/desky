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
    func requestExit() { vm.requestExit() }
    func dismissExit() { vm.dismissExit() }
    func confirmExit() { vm.confirmExit() }
}

struct ExpressionsView: View {
    @StateObject private var state: ExpressionsViewState
    @Binding var selection: DeskyTab

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
        .onAppear { state.onEnter() }
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
