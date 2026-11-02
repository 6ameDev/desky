package com.desky.client.ui.expressions

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.client.ui.components.DeskyTile
import com.desky.client.ui.components.DeskyTileMaxSize
import com.desky.client.ui.components.DeskyTileMinSize
import com.desky.client.ui.components.GapsPerRow
import com.desky.client.ui.components.TilesPerRow
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.control.ExpressionsUiState
import com.desky.shared.features.control.ExpressionsViewModel
import org.koin.androidx.compose.koinViewModel

private const val ExpressionGridColumns = 3
private val ExpressionGridGap = 8.dp

@Composable
fun ExpressionsScreen(
    onExit: () -> Unit,
    vm: ExpressionsViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    // Screen-scope visibility: resumed = enter manual, paused = exit.
    // Scoped to this composable's LifecycleOwner so background/foreground
    // and navigation both funnel through activate / deactivate.
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner, vm) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> vm.activate()
                Lifecycle.Event.ON_PAUSE -> vm.deactivate()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    BackHandler { vm.requestExit() }
    if (state.pendingExit) {
        AlertDialog(
            onDismissRequest = vm::dismissExit,
            title = { Text("Leave expressions?") },
            text = { Text("Unsent expressions will be discarded.") },
            confirmButton = {
                TextButton(
                    onClick = {
                        vm.confirmExit()
                        onExit()
                    },
                ) { Text("Leave") }
            },
            dismissButton = {
                TextButton(onClick = vm::dismissExit) { Text("Stay") }
            },
        )
    }
    ExpressionsContent(
        state = state,
        onTriggerExpression = vm::onTriggerExpression,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ExpressionsContent(
    state: ExpressionsUiState,
    onTriggerExpression: (Int) -> Unit,
) {
    Scaffold(
        topBar = { TopAppBar(title = { Text("Expressions") }) },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            val moodName = state.expressions.firstOrNull { it.id == state.lastTriggeredId }?.name ?: "Neutral"
            Text("Mood: $moodName")
            val entries = state.expressions
            val canTrigger = state.aliveness == Aliveness.Connected && state.telemetry?.manual == true
            if (entries.isEmpty()) return@Column
            BoxWithConstraints(modifier = Modifier.weight(1f)) {
                // Fluid grid tiles: full size on wide phones, shrinking as
                // the viewport narrows so tiles never overflow their cells.
                val tileSize = ((maxWidth - ExpressionGridGap * GapsPerRow) / TilesPerRow)
                    .coerceIn(DeskyTileMinSize, DeskyTileMaxSize)
                LazyVerticalGrid(
                    columns = GridCells.Fixed(ExpressionGridColumns),
                    horizontalArrangement = Arrangement.spacedBy(ExpressionGridGap),
                    verticalArrangement = Arrangement.spacedBy(ExpressionGridGap),
                    modifier = Modifier.fillMaxSize(),
                ) {
                    items(entries, key = { it.id }) { entry ->
                        Box(
                            modifier = Modifier.fillMaxWidth(),
                            contentAlignment = Alignment.Center,
                        ) {
                            DeskyTile(
                                title = entry.name,
                                subtitle = "Priority ${entry.priority}",
                                enabled = canTrigger,
                                onClick = { onTriggerExpression(entry.id) },
                                tileSize = tileSize,
                            )
                        }
                    }
                }
            }
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
    }
}

@Preview
@Composable
private fun ExpressionsContentPreview() {
    MaterialTheme {
        ExpressionsContent(
            state = ExpressionsUiState(aliveness = Aliveness.Connected),
            onTriggerExpression = {},
        )
    }
}
