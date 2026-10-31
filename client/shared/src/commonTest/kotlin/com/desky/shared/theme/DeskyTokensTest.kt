package com.desky.shared.theme

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotEquals

class DeskyTokensTest {

    @Test
    fun lightHexesParseToExpectedLongs() {
        assertEquals(0xFFF4F4F5, DeskyTokens.Light.canvas)
        assertEquals(0xFFFFFFFF, DeskyTokens.Light.card)
        assertEquals(0xFF1E1B16, DeskyTokens.Light.text)
        assertEquals(0xFF6F6A5E, DeskyTokens.Light.secondary)
        assertEquals(0xFFE85D2A, DeskyTokens.Light.primary)
        assertEquals(0xFFD93025, DeskyTokens.Light.danger)
        assertEquals(0xFFE8710A, DeskyTokens.Light.amber)
        assertEquals(0xFF1A8737, DeskyTokens.Light.green)
        assertEquals(0xFF46D8FF, DeskyTokens.Light.glow)
        assertEquals(0xFFE9E6DE, DeskyTokens.Light.island)
        assertEquals(0xFFC8CAD2, DeskyTokens.Light.metal)
        assertEquals(0x33FFFFFF, DeskyTokens.Light.film)
        assertEquals(0x59D93025, DeskyTokens.Light.hit)
    }

    @Test
    fun argbParsesSixAndEightDigitHex() {
        assertEquals(0xFFF4F4F5, DeskyTokens.argb("#F4F4F5"))
        assertEquals(0x33FFFFFF, DeskyTokens.argb("#FFFFFF33"))
        assertEquals(0x59D93025, DeskyTokens.argb("#D9302559"))
        assertEquals(0x66000000, DeskyTokens.argb("#00000066"))
        assertEquals(DeskyTokens.Light.canvas, DeskyTokens.argb("#F4F4F5"))
    }

    @Test
    fun darkDiffersForSurfaceTokens() {
        assertNotEquals(DeskyTokens.Light.canvas, DeskyTokens.Dark.canvas)
        assertNotEquals(DeskyTokens.Light.card, DeskyTokens.Dark.card)
        assertNotEquals(DeskyTokens.Light.text, DeskyTokens.Dark.text)
        assertNotEquals(DeskyTokens.Light.secondary, DeskyTokens.Dark.secondary)
        assertEquals(0xFF131210, DeskyTokens.Dark.canvas)
        assertEquals(0xFF1E1B16, DeskyTokens.Dark.card)
        assertEquals(0xFFF4F4F5, DeskyTokens.Dark.text)
        assertEquals(0xFFA8A094, DeskyTokens.Dark.secondary)
        assertEquals(DeskyTokens.Light.primary, DeskyTokens.Dark.primary)
    }

    @Test
    fun spacingAndRadiusConstants() {
        assertEquals(8, DeskyTokens.Spacing.s8)
        assertEquals(16, DeskyTokens.Spacing.s16)
        assertEquals(24, DeskyTokens.Spacing.s24)
        assertEquals(16, DeskyTokens.Radius.r16)
        assertEquals(18, DeskyTokens.Radius.r18)
        assertEquals(20, DeskyTokens.Radius.r20)
        assertEquals(28, DeskyTokens.Radius.r28)
    }

    @Test
    fun typeScaleConstants() {
        assertEquals(28, DeskyTokens.Type.hSize)
        assertEquals(17, DeskyTokens.Type.bSize)
        assertEquals(13, DeskyTokens.Type.cSize)
    }
}
