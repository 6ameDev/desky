package com.desky.client.ui.pairing

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
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

private val InkColor = Color(0xFF1E1B16)
private val ErrorColor = Color(0xFFD93025)
private val ShuttleGlow = Color(0xFF46D8FF)

private const val PinLength = 6

private const val ShuttleBodyCorner = 6f
private const val ShuttleSmallCorner = 4f
private const val ShuttleHeadCorner = 5f
private const val ShuttleShadowLeft = 0.02f
private const val ShuttleShadowTop = 0.78f
private const val ShuttleShadowWidth = 0.96f
private const val ShuttleShadowHeight = 0.20f
private const val ShuttleBodyLeft = 0.05f
private const val ShuttleBodyTop = 0.32f
private const val ShuttleBodyWidth = 0.90f
private const val ShuttleBodyHeight = 0.54f
private const val ShuttleHeadLeft = 0.68f
private const val ShuttleHeadWidth = 0.20f
private const val ShuttleHeadHeight = 0.44f
private const val ShuttleEyeWidthFraction = 0.22f
private const val ShuttleEyeHeightFraction = 0.55f
private const val ShuttleEyeLeftFraction = 0.62f

data class PairingActions(
    val onPinChange: (String) -> Unit,
    val onPair: () -> Unit,
    val onUnpair: () -> Unit,
    val onOpenScan: () -> Unit,
)

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
        actions = PairingActions(
            onPinChange = vm::onPinChange,
            onPair = vm::onPair,
            onUnpair = vm::onUnpair,
            onOpenScan = onOpenScan,
        ),
    )
}

@Composable
fun PairingContent(
    state: PairingUiState,
    actions: PairingActions,
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
                PairedDeviceBlock(
                    alias = device.alias,
                    accent = onAccent,
                    onUnpair = actions.onUnpair,
                )
            } ?: when (mode) {
                PairingMode.Method -> MethodModeContent(
                    accent = onAccent,
                    onOpenScan = actions.onOpenScan,
                    onShowCode = { mode = PairingMode.Code },
                )
                PairingMode.Code -> CodeModeContent(
                    state = state,
                    accent = onAccent,
                    onPinChange = actions.onPinChange,
                    onPair = actions.onPair,
                    onShowMethod = { mode = PairingMode.Method },
                )
            }
            state.error?.let { PairingErrorCard(message = it) }
            Spacer(modifier = Modifier.weight(if (mode == PairingMode.Method) 2f else 1f))
        }
    }
}

@Composable
private fun PairedDeviceBlock(
    alias: String,
    accent: Color,
    onUnpair: () -> Unit,
) {
    Text("Paired: $alias", color = accent)
    Button(
        onClick = onUnpair,
        shape = RoundedCornerShape(18.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = Color.White,
            contentColor = InkColor,
        ),
    ) { Text("Unpair") }
}

@Composable
private fun ColumnScope.MethodModeContent(
    accent: Color,
    onOpenScan: () -> Unit,
    onShowCode: () -> Unit,
) {
    BotShuttle()
    Spacer(modifier = Modifier.weight(1f))
    QrTileCard(onClick = onOpenScan)
    TextButton(onClick = onShowCode) {
        Text("enter code instead", color = accent)
    }
}

@Composable
private fun CodeModeContent(
    state: PairingUiState,
    accent: Color,
    onPinChange: (String) -> Unit,
    onPair: () -> Unit,
    onShowMethod: () -> Unit,
) {
    Text(
        "Enter code",
        style = MaterialTheme.typography.headlineSmall,
        color = accent,
        textAlign = TextAlign.Center,
    )
    CodeEntryField(
        pin = state.pin,
        onPinChange = onPinChange,
    )
    ConnectButton(state = state, onPair = onPair)
    TextButton(onClick = onShowMethod) {
        Text("scan instead", color = accent)
    }
}

@Composable
private fun ConnectButton(
    state: PairingUiState,
    onPair: () -> Unit,
) {
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
}

@Composable
private fun PairingErrorCard(message: String) {
    Card(
        shape = RoundedCornerShape(18.dp),
        colors = CardDefaults.cardColors(
            containerColor = Color.White,
        ),
    ) {
        Text(
            message,
            color = ErrorColor,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
        )
    }
}

@Composable
private fun QrTileCard(onClick: () -> Unit) {
    val ink = InkColor
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
    val digits = pin.filter(Char::isDigit).take(PinLength)
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
            val d = next.text.filter(Char::isDigit).take(PinLength)
            fieldValue = TextFieldValue(d, TextRange(next.selection.start.coerceIn(0, d.length)))
            if (d != digits) onPinChange(d)
        },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        cursorBrush = SolidColor(Color.Transparent),
        textStyle = TextStyle(color = Color.Transparent, fontSize = 1.sp),
        decorationBox = {
            DigitCellsRow(
                digits = digits,
                selection = fieldValue.selection,
                onSelect = { index ->
                    fieldValue = TextFieldValue(
                        digits,
                        TextRange(index.coerceIn(0, digits.length)),
                    )
                    focusRequester.requestFocus()
                    keyboard?.show()
                },
            )
        },
        modifier = Modifier
            .size(width = 328.dp, height = 56.dp)
            .focusRequester(focusRequester),
    )
}

@Composable
private fun DigitCellsRow(
    digits: String,
    selection: TextRange,
    onSelect: (Int) -> Unit,
) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        repeat(PinLength) { index ->
            val digit = digits.getOrNull(index)?.toString() ?: ""
            val showCursor = selection.collapsed &&
                selection.start == index &&
                index == digits.length
            DigitCell(
                digit = digit,
                showCursor = showCursor,
                onSelect = { onSelect(index) },
            )
        }
    }
}

@Composable
private fun DigitCell(
    digit: String,
    showCursor: Boolean,
    onSelect: () -> Unit,
) {
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
                onSelect()
            },
    ) {
        if (digit.isNotEmpty()) {
            Text(
                digit,
                fontSize = 24.sp,
                fontWeight = FontWeight.Bold,
                color = InkColor,
            )
        } else if (showCursor) {
            CursorBar()
        }
    }
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
            .background(InkColor.copy(alpha = alpha)),
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
        ShuttleArt(offsetX = offsetX, facing = facing)
    }
}

@Composable
private fun ShuttleArt(
    offsetX: Float,
    facing: Float,
) {
    Canvas(
        modifier = Modifier
            .size(width = 60.dp, height = 40.dp)
            .graphicsLayer {
                translationX = offsetX
                scaleX = facing
            },
        onDraw = {
            val corner = androidx.compose.ui.geometry.CornerRadius(ShuttleBodyCorner, ShuttleBodyCorner)
            drawRoundRect(
                InkColor,
                topLeft = androidx.compose.ui.geometry.Offset(
                    size.width * ShuttleShadowLeft,
                    size.height * ShuttleShadowTop,
                ),
                size = androidx.compose.ui.geometry.Size(
                    size.width * ShuttleShadowWidth,
                    size.height * ShuttleShadowHeight,
                ),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(ShuttleSmallCorner, ShuttleSmallCorner),
            )
            drawRoundRect(
                Color.White,
                topLeft = androidx.compose.ui.geometry.Offset(
                    size.width * ShuttleBodyLeft,
                    size.height * ShuttleBodyTop,
                ),
                size = androidx.compose.ui.geometry.Size(
                    size.width * ShuttleBodyWidth,
                    size.height * ShuttleBodyHeight,
                ),
                cornerRadius = corner,
            )
            val headW = size.width * ShuttleHeadWidth
            val headH = size.height * ShuttleHeadHeight
            val headLeft = size.width * ShuttleHeadLeft
            drawRoundRect(
                InkColor,
                topLeft = androidx.compose.ui.geometry.Offset(headLeft, 0f),
                size = androidx.compose.ui.geometry.Size(headW, headH),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(ShuttleHeadCorner, ShuttleHeadCorner),
            )
            val eyeW = headW * ShuttleEyeWidthFraction
            val eyeH = headH * ShuttleEyeHeightFraction
            drawRoundRect(
                ShuttleGlow,
                topLeft = androidx.compose.ui.geometry.Offset(
                    headLeft + headW * ShuttleEyeLeftFraction,
                    (headH - eyeH) / 2f,
                ),
                size = androidx.compose.ui.geometry.Size(eyeW, eyeH),
                cornerRadius = androidx.compose.ui.geometry.CornerRadius(ShuttleSmallCorner, ShuttleSmallCorner),
            )
        },
    )
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
            actions = PairingActions(
                onPinChange = {},
                onPair = {},
                onUnpair = {},
                onOpenScan = {},
            ),
        )
    }
}

@Preview(name = "Dark")
@Composable
private fun PairingContentPreviewDark() {
    DeskyTheme(darkTheme = true) {
        PairingContent(
            state = PairingUiState(pin = "123", isPairing = true, pairingAttempt = 2),
            actions = PairingActions(
                onPinChange = {},
                onPair = {},
                onUnpair = {},
                onOpenScan = {},
            ),
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
                actions = PairingActions(
                    onPinChange = {},
                    onPair = {},
                    onUnpair = {},
                    onOpenScan = {},
                ),
                initialMode = PairingMode.Method,
            )
        }
    }
}
