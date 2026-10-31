import SwiftUI
import shared

/// Expression picker: tap selects, Apply sends one trigger while MANUAL
/// is reported (same gating language as the sticks).
struct ExpressionPicker: View {
    let expressions: [ExpressionEntry]
    let enabled: Bool
    var onTrigger: (Int32) -> Void
    @State private var selectedId: Int32?

    var body: some View {
        VStack(spacing: 4) {
            Text("Expressions")
            if !expressions.isEmpty {
                let selected = selectedId ?? expressions[0].id
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 8) {
                        ForEach(expressions, id: \.id) { entry in
                            if entry.id == selected {
                                Button(entry.name) { selectedId = entry.id }
                                    .buttonStyle(.borderedProminent)
                                    .disabled(!enabled)
                            } else {
                                Button(entry.name) { selectedId = entry.id }
                                    .buttonStyle(.bordered)
                                    .disabled(!enabled)
                            }
                        }
                    }
                }
                Button("Apply") { onTrigger(selected) }
                    .buttonStyle(.borderedProminent)
                    .disabled(!enabled)
            }
        }
    }
}

/// Drag joystick: normalized stick deflection in -1…1, recentered on release.
/// Up-drag is positive throttle (matches Android's `-y / radius`).
/// Disabled gate: gestures move nothing and send nothing.
struct JoystickPad: View {
    var enabled: Bool = true
    var onStick: (_ throttle: Float, _ steering: Float) -> Void
    @State private var knob = CGSize.zero
    private let radius: CGFloat = 80

    var body: some View {
        ZStack {
            Circle()
                .fill(Color.secondary.opacity(0.2))
                .frame(width: radius * 2, height: radius * 2)
            Circle()
                .fill(Color.accentColor)
                .frame(width: 60, height: 60)
                .offset(knob)
        }
        .frame(width: radius * 2, height: radius * 2)
        .opacity(enabled ? 1 : 0.4)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    guard enabled else { return }
                    var dx = value.translation.width
                    var dy = value.translation.height
                    let dist = sqrt(dx * dx + dy * dy)
                    if dist > radius {
                        let scale = radius / dist
                        dx *= scale
                        dy *= scale
                    }
                    knob = CGSize(width: dx, height: dy)
                    onStick(Float(-dy / radius), Float(dx / radius))
                }
                .onEnded { _ in
                    knob = .zero
                    if enabled { onStick(0, 0) }
                }
        )
    }
}
