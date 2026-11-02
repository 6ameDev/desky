package com.desky.shared.core

/**
 * Bottom-tab destinations: Home and Settings only. Drive and Expressions
 * are stack destinations pushed from Home tiles, never tabs.
 */
enum class DeskyTab(val route: String) {
    HOME("home"),
    SETTINGS("settings"),
}
