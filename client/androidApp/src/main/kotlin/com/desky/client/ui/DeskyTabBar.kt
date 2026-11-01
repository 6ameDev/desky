package com.desky.client.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import com.desky.shared.core.DeskyTab

private data class TabItem(
    val tab: DeskyTab,
    val label: String,
    val icon: ImageVector,
)

private val items = listOf(
    TabItem(DeskyTab.HOME, "Home", Icons.Filled.Home),
    TabItem(DeskyTab.SETTINGS, "Settings", Icons.Filled.Settings),
)

/** Clearance tab screens reserve at the bottom so the floating bar never covers content. */
internal const val TAB_BAR_CLEARANCE_DP = 96

private const val TabPillScaleSelected = 1f
private const val TabPillScaleUnselected = 0.96f

/**
 * Floating tab bar for the two functional tabs. Shrink-wrapped around its
 * items: the accent pill covers the whole active tab (icon plus label),
 * echoing the Figma tabbar. Drive and Expressions are Home tiles, never tabs.
 */
@Composable
fun DeskyTabBar(
    current: DeskyTab,
    onTab: (DeskyTab) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier.clip(RoundedCornerShape(24.dp)),
        shape = RoundedCornerShape(24.dp),
        // Island in dark mode so the float separates from the phone-dark page.
        color = if (isSystemInDarkTheme()) {
            MaterialTheme.colorScheme.surfaceVariant
        } else {
            MaterialTheme.colorScheme.surfaceContainer
        },
        tonalElevation = 2.dp,
        shadowElevation = 3.dp,
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            items.forEach { item ->
                TabPill(
                    selected = item.tab == current,
                    label = item.label,
                    icon = item.icon,
                    onClick = { onTab(item.tab) },
                )
            }
        }
    }
}

@Composable
private fun TabPill(
    selected: Boolean,
    label: String,
    icon: ImageVector,
    onClick: () -> Unit,
) {
    val background by animateColorAsState(
        if (selected) MaterialTheme.colorScheme.primary else Color.Transparent,
        label = "tabPillBackground",
    )
    val content by animateColorAsState(
        if (selected) MaterialTheme.colorScheme.surfaceContainer else MaterialTheme.colorScheme.onSurfaceVariant,
        label = "tabPillContent",
    )
    val scale by animateFloatAsState(
        if (selected) TabPillScaleSelected else TabPillScaleUnselected,
        animationSpec = spring(
            dampingRatio = Spring.DampingRatioMediumBouncy,
            stiffness = Spring.StiffnessMedium,
        ),
        label = "tabPillScale",
    )
    Row(
        modifier = Modifier
            .sizeIn(minWidth = 48.dp, minHeight = 52.dp)
            .graphicsLayer(scaleX = scale, scaleY = scale)
            .clip(RoundedCornerShape(18.dp))
            .background(background)
            .selectable(selected = selected, onClick = onClick, role = Role.Tab)
            .padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
    ) {
        Icon(icon, contentDescription = null, tint = content)
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = content,
        )
    }
}
