package com.desky.client.ui.home

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.home.HomeEntry
import com.desky.shared.features.home.HomeUiState
import com.desky.shared.features.home.HomeViewModel
import org.koin.androidx.compose.koinViewModel

@Composable
fun HomeScreen(
    onDrive: () -> Unit,
    onExpressions: () -> Unit,
    onSettings: () -> Unit,
    vm: HomeViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    HomeContent(
        state = state,
        onDrive = onDrive,
        onExpressions = onExpressions,
        onSettings = onSettings,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun HomeContent(
    state: HomeUiState,
    onDrive: () -> Unit,
    onExpressions: () -> Unit,
    onSettings: () -> Unit,
) {
    Scaffold(
        topBar = { TopAppBar(title = { Text("Desky") }) },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            val device = state.pairedDevice
            if (device != null) {
                Text("Paired: ${device.alias}")
            }
            Text("Link: ${state.aliveness}")
            val linkReady = state.aliveness != Aliveness.Unpaired
            state.entries.forEach { entry ->
                when (entry) {
                    HomeEntry.Drive -> Button(
                        onClick = onDrive,
                        enabled = linkReady,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Drive") }
                    HomeEntry.Expressions -> Button(
                        onClick = onExpressions,
                        enabled = linkReady,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Expressions") }
                    HomeEntry.Settings -> Button(
                        onClick = onSettings,
                        modifier = Modifier.fillMaxWidth(),
                    ) { Text("Settings") }
                }
            }
        }
    }
}

@Preview
@Composable
private fun HomeContentPreview() {
    MaterialTheme {
        HomeContent(
            state = HomeUiState(aliveness = Aliveness.Connected),
            onDrive = {},
            onExpressions = {},
            onSettings = {},
        )
    }
}
