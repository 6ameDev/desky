package com.desky.client.ui.driving

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import android.app.Activity
import android.content.pm.ActivityInfo
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
    // Driving is landscape-locked: the stick sits bottom-right and e-stop
    // bottom-left, so hold landscape while visible and restore the
    // manifest portrait lock on leave. Recreation from the switch is safe —
    // ViewModels survive and the nav stack is saved.
    val context = LocalContext.current
    DisposableEffect(context) {
        val activity = context as? Activity
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        onDispose {
            activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        }
    }
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
    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp, vertical = 8.dp),
        ) {
            Box(
                modifier = Modifier.fillMaxWidth(),
                contentAlignment = Alignment.TopCenter,
            ) {
                StatusCard(state = state)
            }
            Spacer(modifier = Modifier.weight(1f))
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 8.dp, bottom = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                EstopButton(state = state, onEmergencyStop = onEmergencyStop, onRelease = onRelease)
                JoystickPad(
                    onStick = onStick,
                    padSizeDp = 160,
                    knobSizeDp = 56,
                    enabled = state.aliveness == Aliveness.Connected && !state.eStopped,
                )
            }
        }
    }
}

@Composable
private fun StatusCard(state: DrivingUiState) {
    Card(
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
        modifier = Modifier
            .fillMaxWidth()
            .widthIn(max = 560.dp),
    ) {
        Column(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            val sessionError = state.error
            if (sessionError != null) {
                Text(sessionError, color = MaterialTheme.colorScheme.error)
            }
            val telemetry = state.telemetry
            if (telemetry != null) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(if (state.aliveness == Aliveness.Connected) "Online" else "Offline")
                    Text("${telemetry.distanceMm} mm")
                    Text(if (telemetry.driving) "Driving" else "Idle")
                }
            } else {
                Text("Waiting for telemetry…")
            }
        }
    }
}

@Composable
private fun EstopButton(
    state: DrivingUiState,
    onEmergencyStop: () -> Unit,
    onRelease: () -> Unit,
) {
    val danger = ButtonDefaults.buttonColors(
        containerColor = MaterialTheme.colorScheme.error,
        contentColor = MaterialTheme.colorScheme.onError,
        disabledContainerColor = MaterialTheme.colorScheme.error.copy(alpha = 0.4f),
        disabledContentColor = MaterialTheme.colorScheme.onError.copy(alpha = 0.7f),
    )
    if (state.eStopped) {
        Button(
            onClick = onRelease,
            colors = danger,
            shape = CircleShape,
            contentPadding = PaddingValues(0.dp),
            modifier = Modifier
                .size(76.dp)
                .semantics { contentDescription = "Emergency stop engaged, tap to release" },
        ) { EstopLetter() }
    } else {
        Button(
            onClick = onEmergencyStop,
            enabled = state.aliveness == Aliveness.Connected,
            colors = danger,
            shape = CircleShape,
            contentPadding = PaddingValues(0.dp),
            modifier = Modifier
                .size(76.dp)
                .semantics { contentDescription = "Emergency stop" },
        ) { EstopLetter() }
    }
}

@Composable
private fun EstopLetter() {
    Text(
        text = "E",
        fontSize = 28.sp,
        fontWeight = FontWeight.Bold,
    )
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
