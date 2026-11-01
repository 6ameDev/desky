package com.desky.client.ui.settings

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.client.ui.TAB_BAR_CLEARANCE_DP
import com.desky.shared.features.settings.SettingsUiState
import com.desky.shared.features.settings.SettingsViewModel
import org.koin.androidx.compose.koinViewModel
import kotlin.math.roundToInt

private val FloatInputRegex = Regex("^-?\\d*\\.?\\d*$")

private const val PERCENT_SCALE = 100
private const val MAX_LED_PROFILE = 7
private const val SPEED_STEP = 0.1f

private fun String.isValidFloatInput(): Boolean = isEmpty() || matches(FloatInputRegex)

private fun snapSpeed(value: Float): Float =
    ((value / SPEED_STEP).roundToInt() * SPEED_STEP).coerceIn(0f, 1f)

@Composable
fun SettingsScreen(
    vm: SettingsViewModel = koinViewModel(),
) {
    val state by vm.state.collectAsStateWithLifecycle()
    SettingsContent(
        state = state,
        onSpeedLimit = vm::onSpeedLimit,
        onLedProfile = vm::onLedProfile,
        onPid = vm::onPid,
        onWifiSsid = vm::onWifiSsid,
        onForgetDevice = vm::onForgetDevice,
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsContent(
    state: SettingsUiState,
    onSpeedLimit: (Float) -> Unit,
    onLedProfile: (Int) -> Unit,
    onPid: (Float, Float, Float) -> Unit,
    onWifiSsid: (String) -> Unit,
    onForgetDevice: () -> Unit,
) {
    val config = state.config
    var showLedSheet by remember { mutableStateOf(false) }
    var showPidSheet by remember { mutableStateOf(false) }
    var showSsidSheet by remember { mutableStateOf(false) }
    var pidP by remember(config.pidP) { mutableStateOf(config.pidP.toString()) }
    var pidI by remember(config.pidI) { mutableStateOf(config.pidI.toString()) }
    var pidD by remember(config.pidD) { mutableStateOf(config.pidD.toString()) }
    var ssid by remember(config.wifiSsid) { mutableStateOf(config.wifiSsid) }

    fun applyPid(): Boolean {
        val p = pidP.toFloatOrNull()
        val i = pidI.toFloatOrNull()
        val d = pidD.toFloatOrNull()
        if (p != null && i != null && d != null) onPid(p, i, d)
        return p != null && i != null && d != null
    }

    Scaffold(
        // Same page background as Home (canvas light, phone-dark card),
        // so the floating tab bar reads identically on both tabs.
        containerColor = if (isSystemInDarkTheme()) {
            MaterialTheme.colorScheme.surfaceContainer
        } else {
            MaterialTheme.colorScheme.background
        },
        topBar = {
            TopAppBar(title = { Text("Settings") })
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 16.dp, vertical = 24.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            // Fixed-height feedback slot: the indicator toggles inside a box
            // that is always present, so saving never shifts the list.
            SavingSlot(isSaving = state.isSaving)
            SectionLabel("General")
            SettingsCard {
                val speedSubtitle = if (state.isSaving) {
                    "${(config.speedLimit * PERCENT_SCALE).toInt()}% · Saving…"
                } else {
                    "${(config.speedLimit * PERCENT_SCALE).toInt()}%"
                }
                SettingTitle(
                    title = "Speed limit",
                    subtitle = speedSubtitle,
                )
                Slider(
                    value = config.speedLimit,
                    onValueChange = { onSpeedLimit(snapSpeed(it)) },
                    valueRange = 0f..1f,
                )
                SettingsDivider()
                SettingRow(
                    title = "LED profile",
                    subtitle = "Profile ${config.ledProfile}",
                    onClick = { showLedSheet = true },
                )
                SettingsDivider()
                SettingRow(
                    title = "PID gains",
                    subtitle = "P ${config.pidP} · I ${config.pidI} · D ${config.pidD}",
                    onClick = { showPidSheet = true },
                )
            }
            SectionLabel("Network")
            SettingsCard {
                SettingRow(
                    title = "Wi-Fi network",
                    subtitle = config.wifiSsid.ifEmpty { "Not set" },
                    onClick = { showSsidSheet = true },
                )
            }
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            SectionLabel("Device")
            SettingsCard {
                DangerRow(
                    title = "Forget device",
                    onClick = onForgetDevice,
                )
            }
            Spacer(modifier = Modifier.height(TAB_BAR_CLEARANCE_DP.dp))
        }
    }

    if (showLedSheet) {
        LedSheet(
            current = config.ledProfile,
            onPick = onLedProfile,
            onDismiss = { showLedSheet = false },
        )
    }
    if (showPidSheet) {
        val pidKeys = KeyboardOptions(keyboardType = KeyboardType.Decimal, imeAction = ImeAction.Done)
        EditorSheet(
            title = "PID gains",
            onDismiss = { showPidSheet = false },
            onApply = {
                if (applyPid()) showPidSheet = false
            },
        ) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = pidP,
                    onValueChange = { if (it.isValidFloatInput()) pidP = it },
                    label = { Text("P") },
                    singleLine = true,
                    keyboardOptions = pidKeys,
                    keyboardActions = KeyboardActions(onDone = { if (applyPid()) showPidSheet = false }),
                    modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = pidI,
                    onValueChange = { if (it.isValidFloatInput()) pidI = it },
                    label = { Text("I") },
                    singleLine = true,
                    keyboardOptions = pidKeys,
                    keyboardActions = KeyboardActions(onDone = { if (applyPid()) showPidSheet = false }),
                    modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = pidD,
                    onValueChange = { if (it.isValidFloatInput()) pidD = it },
                    label = { Text("D") },
                    singleLine = true,
                    keyboardOptions = pidKeys,
                    keyboardActions = KeyboardActions(onDone = { if (applyPid()) showPidSheet = false }),
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
    if (showSsidSheet) {
        EditorSheet(
            title = "Wi-Fi network",
            onDismiss = { showSsidSheet = false },
            onApply = {
                onWifiSsid(ssid)
                showSsidSheet = false
            },
        ) {
            OutlinedTextField(
                value = ssid,
                onValueChange = { ssid = it },
                label = { Text("Wi-Fi SSID") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = {
                    onWifiSsid(ssid)
                    showSsidSheet = false
                }),
                modifier = Modifier.fillMaxWidth(),
            )
        }
    }
}

@Composable
private fun SavingSlot(isSaving: Boolean) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (isSaving) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.labelMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun SettingsCard(content: @Composable androidx.compose.foundation.layout.ColumnScope.() -> Unit) {
    Card(
        shape = RoundedCornerShape(28.dp),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outline),
        modifier = Modifier
            .fillMaxWidth()
            .widthIn(max = 560.dp),
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }
}

@Composable
private fun SettingsDivider() {
    HorizontalDivider(color = MaterialTheme.colorScheme.outline.copy(alpha = 0.5f))
}

@Composable
private fun SettingTitle(title: String, subtitle: String) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(text = title, style = MaterialTheme.typography.bodyLarge)
        Text(
            text = subtitle,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun SettingRow(title: String, subtitle: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 56.dp)
            .clip(RoundedCornerShape(16.dp))
            .clickable(role = Role.Button, onClick = onClick)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge)
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun DangerRow(title: String, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 48.dp)
            .clip(RoundedCornerShape(16.dp))
            .clickable(role = Role.Button, onClick = onClick)
            .padding(vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.error,
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun LedSheet(current: Int, onPick: (Int) -> Unit, onDismiss: () -> Unit) {
    var staged by remember { mutableStateOf(current) }
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier.padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(text = "LED profile", style = MaterialTheme.typography.bodyLarge)
            (0..MAX_LED_PROFILE).forEach { index ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .defaultMinSize(minHeight = 48.dp)
                        .clip(RoundedCornerShape(16.dp))
                        .clickable(role = Role.RadioButton) { staged = index }
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    RadioButton(selected = staged == index, onClick = { staged = index })
                    Text(text = "$index", style = MaterialTheme.typography.bodyLarge)
                }
            }
            Button(
                onClick = {
                    if (staged != current) onPick(staged)
                    onDismiss()
                },
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp),
            ) {
                Text("Done")
            }
            Spacer(modifier = Modifier.height(24.dp))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun EditorSheet(
    title: String,
    onDismiss: () -> Unit,
    onApply: () -> Unit,
    content: @Composable () -> Unit,
) {
    ModalBottomSheet(onDismissRequest = onDismiss) {
        Column(
            modifier = Modifier.padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text(text = title, style = MaterialTheme.typography.bodyLarge)
            content()
            Button(
                onClick = onApply,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = 48.dp),
            ) {
                Text("Apply")
            }
            Spacer(modifier = Modifier.height(24.dp))
        }
    }
}

@Preview
@Composable
private fun SettingsContentPreview() {
    MaterialTheme {
        SettingsContent(
            state = SettingsUiState(),
            onSpeedLimit = {},
            onLedProfile = { _ -> },
            onPid = { _, _, _ -> },
            onWifiSsid = {},
            onForgetDevice = {},
        )
    }
}
