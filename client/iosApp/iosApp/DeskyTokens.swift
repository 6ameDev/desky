import SwiftUI

extension Color {
    init(hex: String) {
        self.init(uiColor: UIColor(argbHex: hex))
    }

    static let deskyCanvas = Color(uiColor: .deskyCanvas)
    static let deskyCard = Color(uiColor: .deskyCard)
    static let deskyText = Color(uiColor: .deskyText)
    static let deskySecondary = Color(uiColor: .deskySecondary)
    static let deskyPrimary = Color(uiColor: .deskyPrimary)
    static let deskyDanger = Color(uiColor: .deskyDanger)
    static let deskyAmber = Color(uiColor: .deskyAmber)
    static let deskyGreen = Color(uiColor: .deskyGreen)
    static let deskyGlow = Color(uiColor: .deskyGlow)
    static let deskyIsland = Color(uiColor: .deskyIsland)
    static let deskyMetal = Color(uiColor: .deskyMetal)
    static let deskyFilm = Color(uiColor: .deskyFilm)
    static let deskyHit = Color(uiColor: .deskyHit)
}

extension UIColor {
    convenience init(argbHex hex: String) {
        let h = hex.hasPrefix("#") ? String(hex.dropFirst()) : hex
        let argb: UInt64
        if h.count == 6 {
            argb = UInt64("FF" + h, radix: 16) ?? 0xFF000000
        } else {
            let rgb = String(h.prefix(6))
            let alpha = String(h.suffix(2))
            argb = UInt64(alpha + rgb, radix: 16) ?? 0xFF000000
        }
        let a = CGFloat((argb >> 24) & 0xFF) / 255
        let r = CGFloat((argb >> 16) & 0xFF) / 255
        let g = CGFloat((argb >> 8) & 0xFF) / 255
        let b = CGFloat(argb & 0xFF) / 255
        self.init(red: r, green: g, blue: b, alpha: a)
    }

    private static func dynamic(light: String, dark: String) -> UIColor {
        UIColor { trait in
            UIColor(argbHex: trait.userInterfaceStyle == .dark ? dark : light)
        }
    }

    static var deskyCanvas: UIColor { .dynamic(light: "#F4F4F5", dark: "#131210") }
    static var deskyCard: UIColor { .dynamic(light: "#FFFFFF", dark: "#1E1B16") }
    static var deskyText: UIColor { .dynamic(light: "#1E1B16", dark: "#F4F4F5") }
    static var deskySecondary: UIColor { .dynamic(light: "#6F6A5E", dark: "#A8A094") }
    static var deskyPrimary: UIColor { .dynamic(light: "#E85D2A", dark: "#E85D2A") }
    static var deskyDanger: UIColor { .dynamic(light: "#D93025", dark: "#FF6B61") }
    static var deskyAmber: UIColor { .dynamic(light: "#E8710A", dark: "#F5A524") }
    static var deskyGreen: UIColor { .dynamic(light: "#1A8737", dark: "#4CAF7D") }
    static var deskyGlow: UIColor { .dynamic(light: "#46D8FF", dark: "#46D8FF") }
    static var deskyIsland: UIColor { .dynamic(light: "#E9E6DE", dark: "#2A2723") }
    static var deskyMetal: UIColor { .dynamic(light: "#C8CAD2", dark: "#3A3D45") }
    static var deskyFilm: UIColor { .dynamic(light: "#FFFFFF33", dark: "#00000066") }
    static var deskyHit: UIColor { .dynamic(light: "#D9302559", dark: "#FF6B6159") }
}

enum DeskySpacing {
    static let s8: CGFloat = 8
    static let s16: CGFloat = 16
    static let s24: CGFloat = 24
}

enum DeskyRadius {
    static let r16: CGFloat = 16
    static let r18: CGFloat = 18
    static let r20: CGFloat = 20
    static let r28: CGFloat = 28
}

enum DeskyType {
    static let hSize: CGFloat = 28
    static let bSize: CGFloat = 17
    static let cSize: CGFloat = 13
}
