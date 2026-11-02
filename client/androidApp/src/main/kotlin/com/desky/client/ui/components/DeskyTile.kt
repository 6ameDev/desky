package com.desky.client.ui.components

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private const val TileDisabledAlpha = 0.6f

/** Largest tile edge: matches the original Home tile width. */
val DeskyTileMaxSize = 106.dp

/** Smallest tile edge before text wraps or clips. */
val DeskyTileMinSize = 84.dp

/** Tiles per row on Home and in the Expressions grid. */
const val TilesPerRow = 3

/** Gaps per row: one fewer than [TilesPerRow]. */
const val GapsPerRow = 2

@Composable
fun DeskyTile(
    title: String,
    subtitle: String,
    enabled: Boolean,
    onClick: () -> Unit,
    tileSize: Dp = DeskyTileMaxSize,
) {
    Card(
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            contentColor = MaterialTheme.colorScheme.onSurface,
            disabledContainerColor = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = TileDisabledAlpha),
            disabledContentColor = MaterialTheme.colorScheme.onSurface.copy(alpha = TileDisabledAlpha),
        ),
        modifier = Modifier
            .size(width = tileSize, height = tileSize)
            .clip(RoundedCornerShape(16.dp))
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp),
                modifier = Modifier.padding(horizontal = 4.dp),
            ) {
                Text(
                    title,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                )
                Text(
                    subtitle,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Normal,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                )
            }
        }
    }
}

@Preview
@Composable
private fun DeskyTilePreview() {
    MaterialTheme {
        DeskyTile(
            title = "Camera",
            subtitle = "Capture Moments",
            enabled = true,
            onClick = {},
        )
    }
}
