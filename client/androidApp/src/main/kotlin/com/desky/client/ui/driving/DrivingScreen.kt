package com.desky.client.ui.driving

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
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
import com.desky.client.ui.control.JoystickPad
import com.desky.shared.features.control.Aliveness
import com.desky.shared.features.control.DrivingUiState
import com.desky.shared.features.control.DrivingViewModel
import org.koin.androidx.compose.koinViewModel

@Composable
fun DrivingScreen(
    onExit: () -> Unit,
    vm: DrivingViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    // Screen-scope visibility: resumed = enter manual, paused = exit.
    // Scoped to this composable's LifecycleOwner so background/foreground
    // and navigation both funnel through activate / deactivate, and other
    // screens are unaffected. Adds while-resumed only — registering while
    // already resumed dispatches ON_RESUME, so first entry is covered with
    // no separate one-shot effect.
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
            title = { Text("Leave driving?") },
            text = { Text("The robot will be released and stop.") },
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
    DrivingContent(
        state = state,
        onStick = vm::onStick,
        onEmergencyStop = vm::onEmergencyStop,
        onRelease = vm::onReleaseEmergency,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun DrivingContent(
    state: DrivingUiState,
    onStick: (Float, Float) -> Unit,
    onEmergencyStop: () -> Unit,
    onRelease: () -> Unit,
) {
    Scaffold(
        topBar = { TopAppBar(title = { Text("Driving") }) },
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
            val sessionError = state.error
            if (sessionError != null) {
                Text(sessionError, color = MaterialTheme.colorScheme.error)
            }
            val telemetry = state.telemetry
            if (telemetry != null) {
                Text("Distance: ${telemetry.distanceMm} mm")
            } else {
                Text("Waiting for telemetry…")
            }
            JoystickPad(
                onStick = onStick,
                enabled = state.aliveness == Aliveness.Connected && !state.eStopped,
            )
            if (state.eStopped) {
                Button(
                    onClick = onRelease,
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                ) { Text("STOPPED — tap to release") }
            } else {
                Button(
                    onClick = onEmergencyStop,
                    enabled = state.aliveness == Aliveness.Connected,
                ) { Text("EMERGENCY STOP") }
            }
            state.telemetry?.let {
                Text(if (it.driving) "Driving" else "Idle")
            }
        }
    }
}

@Preview
@Composable
private fun DrivingContentPreview() {
    MaterialTheme {
        DrivingContent(
            state = DrivingUiState(aliveness = Aliveness.Connected),
            onStick = { _, _ -> },
            onEmergencyStop = {},
            onRelease = {},
        )
    }
}
