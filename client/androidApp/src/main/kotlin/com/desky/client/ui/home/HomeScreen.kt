package com.desky.client.ui.home

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import android.util.Log
import com.desky.client.ui.TAB_BAR_CLEARANCE_DP
import com.desky.client.ui.components.DeskyTile
import com.desky.client.ui.components.DeskyTileMaxSize
import com.desky.client.ui.components.DeskyTileMinSize
import com.desky.client.ui.components.GapsPerRow
import com.desky.client.ui.components.TilesPerRow
import com.desky.client.ui.theme.DeskyTheme
import com.desky.shared.core.RobotMode
import com.desky.shared.core.Telemetry
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.home.HomeEntry
import com.desky.shared.features.home.HomePill
import com.desky.shared.features.home.HomeUiState
import com.desky.shared.features.home.HomeViewModel
import org.koin.androidx.compose.koinViewModel

private val HomeInk = Color(0xFF1E1B16)
private val BotShellColor = Color(0xFFC8CAD2)
private val BotGlowColor = Color(0xFF46D8FF)
private const val BotAspectWidth = 180f
private const val BotAspectHeight = 158f
private const val BotShellCornerFraction = 0.20f
private const val BotFacePadFraction = 0.10f
private const val BotFaceCornerFraction = 0.16f
private const val BotEyeWidthFraction = 0.20f
private const val BotEyeOpenHeightFraction = 0.24f
private const val BotEyeClosedHeightFraction = 0.05f
private const val BotEyeCornerFraction = 0.30f
private const val BotEyeLeftXFraction = 0.275f
private const val BotEyeRightXFraction = 0.525f
private const val PreviewBatteryMv = 850
private val FullTileGap = 16.dp
private val NarrowTileGap = 8.dp

@Composable
fun HomeScreen(
    onDrive: () -> Unit,
    onExpressions: () -> Unit,
    vm: HomeViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    HomeContent(
        state = state,
        onDrive = onDrive,
        onExpressions = onExpressions,
    )
}

@Composable
fun HomeContent(
    state: HomeUiState,
    onDrive: () -> Unit,
    onExpressions: () -> Unit,
) {
    val screen = MaterialTheme.colorScheme.background
    Scaffold(containerColor = screen) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Spacer(modifier = Modifier.weight(1f))
            BotHead(eyesOpen = state.aliveness == Aliveness.Connected)
            PillsRow(pills = state.pills)
            Spacer(modifier = Modifier.height(8.dp))
            val linkReady = state.aliveness != Aliveness.Unpaired
            HomeTileRow(
                entries = state.entries,
                linkReady = linkReady,
                onDrive = onDrive,
                onExpressions = onExpressions,
            )
            Spacer(modifier = Modifier.weight(1f))
            Spacer(modifier = Modifier.height(TAB_BAR_CLEARANCE_DP.dp))
        }
    }
}

@Composable
private fun HomeTileRow(
    entries: List<HomeEntry>,
    linkReady: Boolean,
    onDrive: () -> Unit,
    onExpressions: () -> Unit,
) {
    BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
        // Fluid tiles: full size on wide phones, shrinking with tighter gaps
        // as the viewport narrows so three tiles never clip or scroll.
        val fullTile = (maxWidth - FullTileGap * GapsPerRow) / TilesPerRow
        val gap = if (fullTile >= DeskyTileMaxSize) FullTileGap else NarrowTileGap
        val tileSize = ((maxWidth - gap * GapsPerRow) / TilesPerRow).coerceIn(DeskyTileMinSize, DeskyTileMaxSize)
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(gap, Alignment.CenterHorizontally),
        ) {
            entries.forEach { entry ->
                when (entry) {
                    HomeEntry.Drive -> DeskyTile(
                        title = "Drive",
                        subtitle = if (linkReady) "Manual control" else "Offline",
                        enabled = linkReady,
                        onClick = onDrive,
                        tileSize = tileSize,
                    )
                    HomeEntry.Expressions -> DeskyTile(
                        title = "Expressions",
                        subtitle = if (linkReady) "Play moods" else "Offline",
                        enabled = linkReady,
                        onClick = onExpressions,
                        tileSize = tileSize,
                    )
                    HomeEntry.Settings -> Unit
                }
            }
            DeskyTile(
                title = "Camera",
                subtitle = "Take Snaps",
                enabled = true,
                onClick = { Log.d("HomeScreen", "Camera tile clicked") },
                tileSize = tileSize,
            )
        }
    }
}

@Composable
private fun BotHead(eyesOpen: Boolean) {
    val shell = BotShellColor
    val glow = BotGlowColor
    val screen = HomeInk
    Canvas(modifier = Modifier
        .fillMaxWidth()
        .padding(horizontal = 24.dp)
        .aspectRatio(BotAspectWidth / BotAspectHeight)
    ) {
        val shellW = size.width
        val shellH = size.height
        drawRoundRect(
            shell,
            size = androidx.compose.ui.geometry.Size(shellW, shellH),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(
                shellW * BotShellCornerFraction,
                shellW * BotShellCornerFraction,
            ),
        )
        val padX = shellW * BotFacePadFraction
        val padY = shellH * BotFacePadFraction
        drawRoundRect(
            screen,
            topLeft = androidx.compose.ui.geometry.Offset(padX, padY),
            size = androidx.compose.ui.geometry.Size(shellW - 2 * padX, shellH - 2 * padY),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(
                shellW * BotFaceCornerFraction,
                shellW * BotFaceCornerFraction,
            ),
        )
        val eyeW = shellW * BotEyeWidthFraction
        val eyeH = if (eyesOpen) shellH * BotEyeOpenHeightFraction else shellH * BotEyeClosedHeightFraction
        val eyeY = (shellH - eyeH) / 2f
        val eyeRadius = eyeW * BotEyeCornerFraction
        drawRoundRect(
            glow,
            topLeft = androidx.compose.ui.geometry.Offset(shellW * BotEyeLeftXFraction, eyeY),
            size = androidx.compose.ui.geometry.Size(eyeW, eyeH),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(eyeRadius, eyeRadius),
        )
        drawRoundRect(
            glow,
            topLeft = androidx.compose.ui.geometry.Offset(shellW * BotEyeRightXFraction, eyeY),
            size = androidx.compose.ui.geometry.Size(eyeW, eyeH),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(eyeRadius, eyeRadius),
        )
    }
}

@Composable
private fun PillsRow(pills: List<HomePill>) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.CenterHorizontally),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        pills.forEach { pill ->
            Card(
                shape = RoundedCornerShape(20.dp),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.onSurfaceVariant,
                ),
                modifier = Modifier.semantics {
                    contentDescription = "${pill.label}: ${pill.value}"
                },
            ) {
                Box(
                    modifier = Modifier
                        .background(Color.Transparent)
                        .padding(horizontal = 16.dp, vertical = 10.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        pill.value,
                        color = MaterialTheme.colorScheme.surfaceContainer,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold,
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                    )
                }
            }
        }
    }
}

@Preview(name = "Light")
@Composable
private fun HomeContentPreviewLight() {
    DeskyTheme(darkTheme = false) {
        HomeContent(
            state = HomeUiState(
                aliveness = Aliveness.Connected,
                telemetry = Telemetry(1f, 2f, PreviewBatteryMv, cliff = false, driving = true, mode = RobotMode.MANUAL, manual = true),
            ),
            onDrive = {},
            onExpressions = {},
        )
    }
}

@Preview(name = "Dark")
@Composable
private fun HomeContentPreviewDark() {
    DeskyTheme(darkTheme = true) {
        HomeContent(
            state = HomeUiState(aliveness = Aliveness.Stale),
            onDrive = {},
            onExpressions = {},
        )
    }
}
