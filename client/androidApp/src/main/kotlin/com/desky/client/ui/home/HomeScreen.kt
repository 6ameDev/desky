package com.desky.client.ui.home

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.client.ui.TAB_BAR_CLEARANCE_DP
import com.desky.client.ui.theme.DeskyTheme
import com.desky.shared.core.RobotMode
import com.desky.shared.core.Telemetry
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.home.HomeEntry
import com.desky.shared.features.home.HomePill
import com.desky.shared.features.home.HomeUiState
import com.desky.shared.features.home.HomeViewModel
import org.koin.androidx.compose.koinViewModel

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
    val ink = Color(0xFF1E1B16)
    val screen = if (isSystemInDarkTheme()) {
        MaterialTheme.colorScheme.surfaceContainer
    } else {
        MaterialTheme.colorScheme.background
    }
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
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(
                    16.dp,
                    Alignment.CenterHorizontally,
                ),
            ) {
                state.entries.forEach { entry ->
                    when (entry) {
                        HomeEntry.Drive -> ActionTile(
                            title = "Drive",
                            subtitle = if (linkReady) "Manual control" else "Offline",
                            enabled = linkReady,
                            onClick = onDrive,
                        )
                        HomeEntry.Expressions -> ActionTile(
                            title = "Expressions",
                            subtitle = if (linkReady) "Play moods" else "Offline",
                            enabled = linkReady,
                            onClick = onExpressions,
                        )
                        HomeEntry.Settings -> Unit
                    }
                }
            }
            Spacer(modifier = Modifier.weight(1f))
            Spacer(modifier = Modifier.height(TAB_BAR_CLEARANCE_DP.dp))
        }
    }
}

@Composable
private fun BotHead(eyesOpen: Boolean) {
    val shell = Color(0xFFC8CAD2)
    val glow = Color(0xFF46D8FF)
    val screen = Color(0xFF1E1B16)
    Canvas(modifier = Modifier
        .fillMaxWidth()
        .aspectRatio(180f / 158f)
    ) {
        val shellW = size.width
        val shellH = size.height
        drawRoundRect(
            shell,
            size = androidx.compose.ui.geometry.Size(shellW, shellH),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(
                shellW * 0.20f,
                shellW * 0.20f,
            ),
        )
        val padX = shellW * 0.10f
        val padY = shellH * 0.10f
        drawRoundRect(
            screen,
            topLeft = androidx.compose.ui.geometry.Offset(padX, padY),
            size = androidx.compose.ui.geometry.Size(shellW - 2 * padX, shellH - 2 * padY),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(
                shellW * 0.16f,
                shellW * 0.16f,
            ),
        )
        val eyeW = shellW * 0.20f
        val eyeH = if (eyesOpen) shellH * 0.24f else shellH * 0.05f
        val eyeY = (shellH - eyeH) / 2f
        val eyeRadius = eyeW * 0.30f
        drawRoundRect(
            glow,
            topLeft = androidx.compose.ui.geometry.Offset(shellW * 0.275f, eyeY),
            size = androidx.compose.ui.geometry.Size(eyeW, eyeH),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(eyeRadius, eyeRadius),
        )
        drawRoundRect(
            glow,
            topLeft = androidx.compose.ui.geometry.Offset(shellW * 0.525f, eyeY),
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

@Composable
private fun ActionTile(
    title: String,
    subtitle: String,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Card(
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = Color.White,
            contentColor = Color(0xFF1E1B16),
            disabledContainerColor = Color.White.copy(alpha = 0.6f),
            disabledContentColor = Color(0xFF1E1B16).copy(alpha = 0.6f),
        ),
        modifier = Modifier
            .size(width = 106.dp, height = 104.dp)
            .clip(RoundedCornerShape(16.dp))
            .clickable(enabled = enabled, onClick = onClick),
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp),
                modifier = Modifier.padding(horizontal = 8.dp),
            ) {
                Text(
                    title,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                )
                Text(
                    subtitle,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Normal,
                    color = Color(0xFF6F6A5E),
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                )
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
                telemetry = Telemetry(1f, 2f, 850, cliff = false, driving = true, mode = RobotMode.MANUAL, manual = true),
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
