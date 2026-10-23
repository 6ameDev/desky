package com.desky.client.ui.control

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.shared.domain.RobotMode
import com.desky.shared.domain.Telemetry
import com.desky.shared.presentation.Connectivity
import com.desky.shared.presentation.RobotControlUiState
import com.desky.shared.presentation.RobotControlViewModel
import org.koin.androidx.compose.koinViewModel
import kotlin.math.roundToInt

@Composable
fun ControlScreen(
    onOpenSettings: () -> Unit,
    vm: RobotControlViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    // Screen-scope lifecycle: leaving Controls = goodbye (flush + DISCONNECT 2x
    // + pause via onBackground), returning = immediate ALIVE + resume via
    // onForeground. Scoped to this composable's LifecycleOwner so other screens
    // are unaffected (NOT ProcessLifecycleOwner — app-wide would pause while the
    // user is merely on Settings). Rapid pause/resume needs no extra debounce
    // here: onForeground's ALIVE due-flag is idempotent (bursts collapse to one
    // ALIVE) and the background-pause flag is a plain boolean.
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner, vm) {
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_PAUSE -> vm.onBackground()
                Lifecycle.Event.ON_RESUME -> vm.onForeground()
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }
    ControlContent(
        state = state,
        onStick = vm::onStick,
        onEmergencyStop = vm::onEmergencyStop,
        onReleaseEmergency = vm::onReleaseEmergency,
        onTakeManual = vm::onTakeManual,
        onReleaseManual = vm::onReleaseManual,
        onOpenSettings = onOpenSettings,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ControlContent(
    state: RobotControlUiState,
    onStick: (Float, Float) -> Unit,
    onEmergencyStop: () -> Unit,
    onReleaseEmergency: () -> Unit,
    onOpenSettings: () -> Unit,
    onTakeManual: () -> Unit = {},
    onReleaseManual: () -> Unit = {},
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Controls") },
                actions = {
                    IconButton(onClick = onOpenSettings) {
                        Icon(Icons.Filled.Settings, contentDescription = "Settings")
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            TelemetryBar(state)
            // Disconnected gate (mirrors the ViewModel no-op guards on
            // onStick/onEmergencyStop/onReleaseEmergency): the robot's
            // ~500 ms stale-command failsafe is already stopping it, and the
            // ALIVE knock underneath re-enables on the first telemetry.
            val controlsEnabled = state.connectivity != Connectivity.Disconnected
            // Manual entry: paired no longer implies drivable — the drive
            // loop only sends while the robot reports MANUAL (take/release
            // below); the joystick stays inert until the hold is reported.
            val manualControls = controlsEnabled && state.manualMode
            if (state.manualMode) {
                OutlinedButton(onClick = onReleaseManual, enabled = manualControls) { Text("Release Manual") }
            } else {
                Button(onClick = onTakeManual, enabled = controlsEnabled) { Text("Take Manual") }
            }
            JoystickPad(onStick = onStick, enabled = manualControls)
            if (state.eStopped) {
                OutlinedButton(onClick = onReleaseEmergency, enabled = controlsEnabled) { Text("Release E-STOP") }
            } else {
                Button(
                    onClick = onEmergencyStop,
                    enabled = controlsEnabled,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = MaterialTheme.colorScheme.error,
                    ),
                ) { Text("E-STOP") }
            }
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
        }
    }
}

@Composable
private fun TelemetryBar(state: RobotControlUiState) {
    val telemetry = state.telemetry
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        Text("Link: ${state.connectivity}  Mode: ${state.mode}")
        Text(if (state.manualMode) "Manual: active" else "Manual: inactive")
        if (telemetry != null) {
            Text("Distance: ${telemetry.distanceMm} mm")
            Text("Pitch: ${telemetry.pitchDeg}°  Roll: ${telemetry.rollDeg}°")
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(if (telemetry.cliff) "CLIFF!" else "Cliff: clear")
                Text(if (telemetry.driving) "Driving" else "Idle")
            }
        } else {
            Text("Waiting for telemetry…")
        }
    }
}

@Composable
fun JoystickPad(
    onStick: (throttle: Float, steering: Float) -> Unit,
    modifier: Modifier = Modifier,
    padSizeDp: Int = 192,
    knobSizeDp: Int = 64,
    /** Disconnected gate: while false, gestures move nothing and send nothing. */
    enabled: Boolean = true,
) {
    var knob by remember { mutableStateOf(Offset.Zero) }
    Box(
        modifier = modifier
            .size(padSizeDp.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant, CircleShape)
            .pointerInput(enabled) {
                detectDragGestures(
                    onDragStart = { offset ->
                        if (enabled) {
                            val radius = size.width / 2f
                            val centered = offset - Offset(radius, radius)
                            val clamped = clampToRadius(centered, radius)
                            knob = clamped
                            onStick(-clamped.y / radius, clamped.x / radius)
                        }
                    },
                    onDrag = { change, _ ->
                        if (enabled) {
                            change.consume()
                            val radius = size.width / 2f
                            val centered = change.position - Offset(radius, radius)
                            val clamped = clampToRadius(centered, radius)
                            knob = clamped
                            onStick(-clamped.y / radius, clamped.x / radius)
                        }
                    },
                    onDragEnd = {
                        knob = Offset.Zero
                        if (enabled) onStick(0f, 0f)
                    },
                    onDragCancel = {
                        knob = Offset.Zero
                        if (enabled) onStick(0f, 0f)
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        val knobOffsetPx = knob
        Box(
            modifier = Modifier
                .offset {
                    IntOffset(knobOffsetPx.x.roundToInt(), knobOffsetPx.y.roundToInt())
                }
                .size(knobSizeDp.dp)
                .background(MaterialTheme.colorScheme.primary, CircleShape),
        )
    }
}

private fun clampToRadius(offset: Offset, radius: Float): Offset {
    val dist = offset.getDistance()
    if (dist <= radius || dist == 0f) return offset
    return offset * (radius / dist)
}

@Preview
@Composable
private fun ControlContentPreview() {
    MaterialTheme {
        ControlContent(
            state = RobotControlUiState(
                telemetry = Telemetry(
                    pitchDeg = 1.5f,
                    rollDeg = -0.5f,
                    distanceMm = 1234,
                    cliff = false,
                    driving = true,
                    mode = RobotMode.MANUAL,
                ),
                connectivity = Connectivity.Connected,
                mode = RobotMode.MANUAL,
            ),
            onStick = { _, _ -> },
            onEmergencyStop = {},
            onReleaseEmergency = {},
            onOpenSettings = {},
        )
    }
}
