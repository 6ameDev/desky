package com.desky.client.ui.expressions

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
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
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.control.ExpressionsUiState
import com.desky.shared.features.control.ExpressionsViewModel
import org.koin.androidx.compose.koinViewModel

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
            Text("Link: ${state.aliveness}")
            val entries = state.expressions
            val canTrigger = state.aliveness == Aliveness.Connected && state.telemetry?.manual == true
            if (entries.isEmpty()) return@Column
            LazyVerticalGrid(
                columns = GridCells.Fixed(3),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.weight(1f),
            ) {
                items(entries, key = { it.id }) { entry ->
                    ExpressionTile(
                        name = entry.name,
                        enabled = canTrigger,
                        onClick = { onTriggerExpression(entry.id) },
                    )
                }
            }
            state.lastTriggeredId?.let { lastId -> Text("Last: ${entries.firstOrNull { it.id == lastId }?.name}") }
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
    }
}

@Composable
private fun ExpressionTile(
    name: String,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Card(
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
            contentColor = MaterialTheme.colorScheme.onSurface,
            disabledContainerColor = MaterialTheme.colorScheme.surfaceContainer.copy(alpha = 0.6f),
            disabledContentColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.6f),
        ),
        modifier = Modifier
            .defaultMinSize(minHeight = 88.dp)
            .clip(RoundedCornerShape(16.dp))
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .padding(8.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                name,
                style = MaterialTheme.typography.bodyLarge,
                textAlign = TextAlign.Center,
                maxLines = 3,
            )
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
