package com.desky.client.ui.expressions

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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

private const val VIRTUAL_COUNT_MULTIPLIER = 1000

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
            var selectedIndex by remember { mutableIntStateOf(0) }
            val selected = selectedIndex.coerceIn(entries.indices)
            val virtualCount = entries.size * VIRTUAL_COUNT_MULTIPLIER
            val startIndex = virtualCount / 2 - (virtualCount / 2 % entries.size)
            val listState = rememberLazyListState(initialFirstVisibleItemIndex = startIndex)
            LazyRow(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                state = listState,
            ) {
                items(virtualCount) { virtualIndex ->
                    val index = virtualIndex % entries.size
                    val entry = entries[index]
                    FilterChip(
                        selected = index == selected,
                        onClick = { selectedIndex = index },
                        enabled = canTrigger,
                        label = { Text(entry.name) },
                    )
                }
            }
            Button(
                onClick = { onTriggerExpression(entries[selected].id) },
                enabled = canTrigger,
            ) { Text("Apply") }
            state.lastTriggeredId?.let { lastId -> Text("Last: ${entries.firstOrNull { it.id == lastId }?.name}") }
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
