package com.desky.client.ui.pairing

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.client.ui.theme.DeskyTheme
import com.desky.client.ui.theme.deskyExtras
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ROBOT_HOST
import com.desky.shared.features.pairing.PairingUiState
import com.desky.shared.features.pairing.PairingViewModel
import org.koin.androidx.compose.koinViewModel

enum class PairingMode {
    Method,
    Code,
}

@Composable
fun PairingScreen(
    onPaired: () -> Unit,
    onOpenScan: () -> Unit,
    vm: PairingViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    LaunchedEffect(state.pairedDevice) {
        if (state.pairedDevice != null) onPaired()
    }
    PairingContent(
        state = state,
        onPinChange = vm::onPinChange,
        onPair = vm::onPair,
        onUnpair = vm::onUnpair,
        onOpenScan = onOpenScan,
    )
}

@Composable
fun PairingContent(
    state: PairingUiState,
    onPinChange: (String) -> Unit,
    onPair: () -> Unit,
    onUnpair: () -> Unit,
    onOpenScan: () -> Unit,
    initialMode: PairingMode = PairingMode.Method,
) {
    var mode by remember { mutableStateOf(initialMode) }
    val onAccent = Color.White
    Scaffold(containerColor = MaterialTheme.colorScheme.primary) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .imePadding()
                .padding(horizontal = 16.dp, vertical = 20.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            if (state.isPairing) {
                StepperDots(step = if (state.pairingAttempt <= 1) 1 else 2)
            }
            Spacer(modifier = Modifier.weight(1f))
            state.pairedDevice?.let { device ->
                Text("Paired: ${device.alias}", color = onAccent)
                Button(
                    onClick = onUnpair,
                    shape = RoundedCornerShape(18.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color.White,
                        contentColor = Color(0xFF1E1B16),
                    ),
                ) { Text("Unpair") }
            } ?: when (mode) {
                PairingMode.Method -> {
                    BotShuttle()
                    Spacer(modifier = Modifier.weight(1f))
                    QrTileCard(onClick = onOpenScan)
                    TextButton(onClick = { mode = PairingMode.Code }) {
                        Text("enter code instead", color = onAccent)
                    }
                }
                PairingMode.Code -> {
                    Text(
                        "Enter code",
                        style = MaterialTheme.typography.headlineSmall,
                        color = onAccent,
                        textAlign = TextAlign.Center,
                    )
                    CodeEntryField(
                        pin = state.pin,
                        onPinChange = onPinChange,
                    )
                    Button(
                        onClick = onPair,
                        enabled = !state.isPairing && state.pairedDevice == null,
                        shape = RoundedCornerShape(28.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = Color.White,
                            contentColor = MaterialTheme.colorScheme.primary,
                            disabledContainerColor = Color.White.copy(alpha = 0.6f),
                            disabledContentColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f),
                        ),
                        modifier = Modifier
                            .width(136.dp)
                            .height(49.dp),
                    ) {
                        Text(
                            text = if (state.isPairing) {
                                if (state.pairingAttempt <= 1) {
                                    "Pairing…"
                                } else {
                                    "Pairing… (attempt ${state.pairingAttempt})"
                                }
                            } else {
                                "Connect"
                            },
                            fontSize = 17.sp,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                    TextButton(onClick = { mode = PairingMode.Method }) {
                        Text("scan instead", color = onAccent)
                    }
                }
            }
            state.error?.let {
                Card(
                    shape = RoundedCornerShape(18.dp),
                    colors = CardDefaults.cardColors(
                        containerColor = Color.White,
                    ),
                ) {
                    Text(
                        it,
                        color = Color(0xFFD93025),
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
                    )
                }
            }
            Spacer(modifier = Modifier.weight(if (mode == PairingMode.Method) 2f else 1f))
        }
    }
}

@Composable
private fun QrTileCard(onClick: () -> Unit) {
    val ink = Color(0xFF1E1B16)
    Card(
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(
            containerColor = Color.White,
            contentColor = ink,
        ),
        modifier = Modifier
            .size(170.dp)
            .clickable(onClick = onClick),
    ) {
        Box(
            modifier = Modifier.fillMaxSize(),
            contentAlignment = Alignment.Center,
        ) {
            Box(
                modifier = Modifier
                    .size(130.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color.Black),
            ) {
                FinderSquare(modifier = Modifier.align(Alignment.TopStart))
                FinderSquare(modifier = Modifier.align(Alignment.TopEnd))
                FinderSquare(modifier = Modifier.align(Alignment.BottomStart))
                FinderSquare(modifier = Modifier.align(Alignment.BottomEnd))
                Text(
                    "scan me",
                    color = Color.White,
                    fontSize = 16.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.align(Alignment.Center),
                )
            }
        }
    }
}

@Composable
private fun FinderSquare(modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .padding(10.dp)
            .size(24.dp)
            .clip(RoundedCornerShape(4.dp))
            .background(Color.White),
    )
}

@Composable
private fun CodeEntryField(
    pin: String,
    onPinChange: (String) -> Unit,
) {
    val focusRequester = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    val digits = pin.filter(Char::isDigit).take(6)
    var fieldValue by remember {
        mutableStateOf(TextFieldValue(digits, TextRange(digits.length)))
    }
    if (fieldValue.text != digits) {
        fieldValue = TextFieldValue(
            digits,
            TextRange(fieldValue.selection.start.coerceIn(0, digits.length)),
        )
    }
    LaunchedEffect(Unit) { focusRequester.requestFocus() }
    BasicTextField(
        value = fieldValue,
        onValueChange = { next ->
            val d = next.text.filter(Char::isDigit).take(6)
            fieldValue = TextFieldValue(d, TextRange(next.selection.start.coerceIn(0, d.length)))
            if (d != digits) onPinChange(d)
        },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        cursorBrush = SolidColor(Color.Transparent),
        textStyle = TextStyle(color = Color.Transparent, fontSize = 1.sp),
        decorationBox = {
            Row(
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                repeat(6) { index ->
                    val digit = digits.getOrNull(index)?.toString() ?: ""
                    val showCursor = fieldValue.selection.collapsed &&
                        fieldValue.selection.start == index &&
                        index == digits.length
                    Box(
                        contentAlignment = Alignment.Center,
                        modifier = Modifier
                            .size(width = 48.dp, height = 56.dp)
                            .clip(RoundedCornerShape(12.dp))
                            .background(Color.White)
                            .clickable(
                                indication = null,
                                interactionSource = remember { MutableInteractionSource() },
                            ) {
                                fieldValue = TextFieldValue(
                                    digits,
                                    TextRange(index.coerceIn(0, digits.length)),
                                )
                                focusRequester.requestFocus()
                                keyboard?.show()
                            },
                    ) {
                        if (digit.isNotEmpty()) {
                            Text(
                                digit,
                                fontSize = 24.sp,
                                fontWeight = FontWeight.Bold,
                                color = Color(0xFF1E1B16),
                            )
                        } else if (showCursor) {
                            CursorBar()
                        }
                    }
                }
            }
        },
        modifier = Modifier
            .size(width = 328.dp, height = 56.dp)
            .focusRequester(focusRequester),
    )
}

@Composable
private fun CursorBar() {
    val alpha by rememberInfiniteTransition(label = "cursor").animateFloat(
        initialValue = 1f,
        targetValue = 0f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 530),
            repeatMode = RepeatMode.Restart,
        ),
        label = "blink",
    )
    Box(
        modifier = Modifier
            .width(2.dp)
            .height(28.dp)
            .background(Color(0xFF1E1B16).copy(alpha = alpha)),
    )
}

@Composable
private fun StepperDots(step: Int) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StepDot(active = step == 1)
        StepDot(active = step == 2)
    }
}

@Composable
private fun StepDot(active: Boolean) {
    val color = if (active) Color.White else Color.White.copy(alpha = 0.35f)
    Canvas(modifier = Modifier.size(8.dp), onDraw = { drawCircle(color) })
}

@Composable
private fun BotShuttle() {
    val body = Color.White
    val dark = Color(0xFF1E1B16)
    val glow = Color(0xFF46D8FF)
    BoxWithConstraints(
        modifier = Modifier.fillMaxWidth(),
        contentAlignment = Alignment.Center,
    ) {
        val amplitudePx = with(LocalDensity.current) {
            (((maxWidth - 60.dp) / 2 - 8.dp).coerceAtLeast(16.dp)).toPx()
        }
        val loop = rememberInfiniteTransition(label = "shuttle")
        val phase by loop.animateFloat(
            initialValue = 0f,
            targetValue = 2f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 8000, easing = LinearEasing),
                repeatMode = RepeatMode.Restart,
            ),
            label = "phase",
        )
        val facing = if (phase < 1f) 1f else -1f
        val offsetX = if (phase < 1f) {
            -amplitudePx + 2f * amplitudePx * phase
        } else {
            amplitudePx - 2f * amplitudePx * (phase - 1f)
        }
        Canvas(
            modifier = Modifier
                .size(width = 60.dp, height = 40.dp)
                .graphicsLayer {
                    translationX = offsetX
                    scaleX = facing
                },
            onDraw = {
            val corner = androidx.compose.ui.geometry.CornerRadius(6f, 6f)
            drawRoundRect(
                dark,
                topLeft = androidx.compose.ui.geometry.Offset(size.width * 0.02f, size.height * 0.78f),
                size = androidx.compose.ui.geometry.Size(size.width * 0.96f, size.height * 0.20f),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(4f, 4f),
            )
            drawRoundRect(
                body,
                topLeft = androidx.compose.ui.geometry.Offset(size.width * 0.05f, size.height * 0.32f),
                size = androidx.compose.ui.geometry.Size(size.width * 0.90f, size.height * 0.54f),
                cornerRadius = corner,
            )
            val headW = size.width * 0.20f
            val headH = size.height * 0.44f
            val headLeft = size.width * 0.68f
            drawRoundRect(
                dark,
                topLeft = androidx.compose.ui.geometry.Offset(headLeft, 0f),
                size = androidx.compose.ui.geometry.Size(headW, headH),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(5f, 5f),
            )
            val eyeW = headW * 0.22f
            val eyeH = headH * 0.55f
            drawRoundRect(
                glow,
                topLeft = androidx.compose.ui.geometry.Offset(
                    headLeft + headW * 0.62f,
                    (headH - eyeH) / 2f,
                ),
                size = androidx.compose.ui.geometry.Size(eyeW, eyeH),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(4f, 4f),
            )
        },
    )
    }
}

@Preview(name = "Light")
@Composable
private fun PairingContentPreviewLight() {
    DeskyTheme(darkTheme = false) {
        PairingContent(
            state = PairingUiState(
                pin = "123456",
                pairedDevice = PairedDevice(
                    id = "1",
                    ip = ROBOT_HOST,
                    authToken = "psk-v1",
                ),
            ),
            onPinChange = {},
            onPair = {},
            onUnpair = {},
            onOpenScan = {},
        )
    }
}

@Preview(name = "Dark")
@Composable
private fun PairingContentPreviewDark() {
    DeskyTheme(darkTheme = true) {
        PairingContent(
            state = PairingUiState(pin = "123", isPairing = true, pairingAttempt = 2),
            onPinChange = {},
            onPair = {},
            onUnpair = {},
            onOpenScan = {},
            initialMode = PairingMode.Code,
        )
    }
}

@Preview(name = "Method")
@Composable
private fun PairingContentPreviewMethod() {
    DeskyTheme(darkTheme = false) {
        Box {
            PairingContent(
                state = PairingUiState(),
                onPinChange = {},
                onPair = {},
                onUnpair = {},
                onOpenScan = {},
                initialMode = PairingMode.Method,
            )
        }
    }
}
