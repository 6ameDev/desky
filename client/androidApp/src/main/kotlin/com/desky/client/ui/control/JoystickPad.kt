package com.desky.client.ui.control

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

/// Drag joystick: normalized stick deflection in -1..1, recentered on release.
/// Up-drag is positive throttle. Disabled gate: gestures move nothing and
/// send nothing.
@Composable
fun JoystickPad(
    onStick: (throttle: Float, steering: Float) -> Unit,
    modifier: Modifier = Modifier,
    padSizeDp: Int = 192,
    knobSizeDp: Int = 64,
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
private fun JoystickPadPreview() {
    MaterialTheme {
        JoystickPad(onStick = { _, _ -> })
    }
}
