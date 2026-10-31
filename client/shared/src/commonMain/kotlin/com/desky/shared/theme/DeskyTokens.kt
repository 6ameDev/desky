package com.desky.shared.theme

data class DeskyPalette(
    val canvas: Long,
    val card: Long,
    val text: Long,
    val secondary: Long,
    val primary: Long,
    val danger: Long,
    val amber: Long,
    val green: Long,
    val glow: Long,
    val island: Long,
    val metal: Long,
    val film: Long,
    val hit: Long,
)

object DeskyTokens {
    val Light = DeskyPalette(
        canvas = 0xFFF4F4F5,
        card = 0xFFFFFFFF,
        text = 0xFF1E1B16,
        secondary = 0xFF6F6A5E,
        primary = 0xFFE85D2A,
        danger = 0xFFD93025,
        amber = 0xFFE8710A,
        green = 0xFF1A8737,
        glow = 0xFF46D8FF,
        island = 0xFFE9E6DE,
        metal = 0xFFC8CAD2,
        film = 0x33FFFFFF,
        hit = 0x59D93025,
    )

    /**
     * Dark is derived, not from Figma (v3 collection is Light-only).
     * Danger/amber/green are lightened to hold contrast against dark canvas.
     */
    val Dark = DeskyPalette(
        canvas = 0xFF131210,
        card = 0xFF1E1B16,
        text = 0xFFF4F4F5,
        secondary = 0xFFA8A094,
        primary = 0xFFE85D2A,
        danger = 0xFFFF6B61,
        amber = 0xFFF5A524,
        green = 0xFF4CAF7D,
        glow = 0xFF46D8FF,
        island = 0xFF2A2723,
        metal = 0xFF3A3D45,
        film = 0x66000000,
        hit = 0x59FF6B61,
    )

    object Spacing {
        const val s8 = 8
        const val s16 = 16
        const val s24 = 24
    }

    object Radius {
        const val r16 = 16
        const val r18 = 18
        const val r20 = 20
        const val r28 = 28
    }

    object Type {
        const val hSize = 28
        const val hWeight = 700
        const val bSize = 17
        const val bWeight = 400
        const val cSize = 13
        const val cWeight = 500
        const val subSize = 16
        const val subWeight = 400
    }

    fun argb(hex: String): Long {
        val h = hex.removePrefix("#")
        require(h.length == 6 || h.length == 8) { "Expected #RRGGBB or #RRGGBBAA" }
        val aarrggbb = if (h.length == 6) "FF$h" else h.substring(6, 8) + h.substring(0, 6)
        return aarrggbb.toLong(16)
    }
}
