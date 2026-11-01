import SwiftUI
import shared

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
        .onChange(of: enabled) { newValue in
            if !newValue {
                knob = .zero
                onStick(0, 0)
            }
        }
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
