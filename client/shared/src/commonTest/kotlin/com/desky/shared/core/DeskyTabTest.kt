package com.desky.shared.core

import kotlin.test.Test
import kotlin.test.assertEquals

class DeskyTabTest {

    @Test
    fun tabsAreHomeAndSettingsOnly() {
        assertEquals(listOf(DeskyTab.HOME, DeskyTab.SETTINGS), DeskyTab.entries)
    }

    @Test
    fun tabRoutesAreDistinct() {
        assertEquals(DeskyTab.entries.size, DeskyTab.entries.map { it.route }.toSet().size)
    }
}
