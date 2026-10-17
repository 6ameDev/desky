package com.desky.shared.domain

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class QrCodeParserTest {

    @Test
    fun acceptsBareSixDigits() {
        assertEquals("123456", QrCodeParser.parse("123456"))
        assertEquals("000000", QrCodeParser.parse("000000"))
        assertEquals("999999", QrCodeParser.parse("999999"))
    }

    @Test
    fun trimsSurroundingWhitespace() {
        assertEquals("123456", QrCodeParser.parse("  123456  "))
        assertEquals("123456", QrCodeParser.parse("\t123456\n"))
    }

    @Test
    fun rejectsWrongLength() {
        assertNull(QrCodeParser.parse(""))
        assertNull(QrCodeParser.parse("12345"))
        assertNull(QrCodeParser.parse("1234567"))
    }

    @Test
    fun rejectsNonDigits() {
        assertNull(QrCodeParser.parse("12345a"))
        assertNull(QrCodeParser.parse("abcdef"))
        // Full-width / non-ASCII digits are not ASCII 0-9.
        assertNull(QrCodeParser.parse("１２３４５６"))
        assertNull(QrCodeParser.parse("12 456"))
        assertNull(QrCodeParser.parse("12-456"))
    }

    @Test
    fun rejectsUrlAndDeepLinkFormsForLater() {
        assertNull(QrCodeParser.parse("desky://pair?code=123456"))
        assertNull(QrCodeParser.parse("https://desky.example/pair/123456"))
        assertNull(QrCodeParser.parse("DESKY:123456"))
    }
}
