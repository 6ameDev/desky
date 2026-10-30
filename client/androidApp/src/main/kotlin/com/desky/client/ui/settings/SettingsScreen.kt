package com.desky.client.ui.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.shared.features.settings.SettingsUiState
import com.desky.shared.features.settings.SettingsViewModel
import org.koin.androidx.compose.koinViewModel

private val FloatInputRegex = Regex("^-?\\d*\\.?\\d*$")

private const val PERCENT_SCALE = 100
private const val MAX_LED_PROFILE = 7

private fun String.isValidFloatInput(): Boolean = isEmpty() || matches(FloatInputRegex)

@Composable
fun SettingsScreen(
    onBack: () -> Unit,
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
        onBack = onBack,
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
    onBack: () -> Unit,
) {
    val config = state.config
    var pidP by remember(config.pidP) { mutableStateOf(config.pidP.toString()) }
    var pidI by remember(config.pidI) { mutableStateOf(config.pidI.toString()) }
    var pidD by remember(config.pidD) { mutableStateOf(config.pidD.toString()) }
    var ssid by remember(config.wifiSsid) { mutableStateOf(config.wifiSsid) }

    fun applyPid() {
        val p = pidP.toFloatOrNull()
        val i = pidI.toFloatOrNull()
        val d = pidD.toFloatOrNull()
        if (p != null && i != null && d != null) onPid(p, i, d)
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
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
        ) {
            if (state.isSaving) LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
            Text("Speed limit: ${(config.speedLimit * PERCENT_SCALE).toInt()}%")
            Slider(
                value = config.speedLimit,
                onValueChange = onSpeedLimit,
                valueRange = 0f..1f,
            )
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("LED profile: ${config.ledProfile}")
                OutlinedButton(
                    onClick = { onLedProfile((config.ledProfile - 1).coerceIn(0, MAX_LED_PROFILE)) },
                    enabled = config.ledProfile > 0,
                ) { Text("−") }
                OutlinedButton(
                    onClick = { onLedProfile((config.ledProfile + 1).coerceIn(0, MAX_LED_PROFILE)) },
                    enabled = config.ledProfile < MAX_LED_PROFILE,
                ) { Text("+") }
            }
            val pidKeys = KeyboardOptions(keyboardType = KeyboardType.Decimal, imeAction = ImeAction.Done)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = pidP,
                    onValueChange = { if (it.isValidFloatInput()) pidP = it },
                    label = { Text("P") },
                    singleLine = true,
                    keyboardOptions = pidKeys,
                    keyboardActions = KeyboardActions(onDone = { applyPid() }),
                    modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = pidI,
                    onValueChange = { if (it.isValidFloatInput()) pidI = it },
                    label = { Text("I") },
                    singleLine = true,
                    keyboardOptions = pidKeys,
                    keyboardActions = KeyboardActions(onDone = { applyPid() }),
                    modifier = Modifier.weight(1f),
                )
                OutlinedTextField(
                    value = pidD,
                    onValueChange = { if (it.isValidFloatInput()) pidD = it },
                    label = { Text("D") },
                    singleLine = true,
                    keyboardOptions = pidKeys,
                    keyboardActions = KeyboardActions(onDone = { applyPid() }),
                    modifier = Modifier.weight(1f),
                )
            }
            OutlinedTextField(
                value = ssid,
                onValueChange = { ssid = it },
                label = { Text("Wi-Fi SSID") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { onWifiSsid(ssid) }),
                modifier = Modifier.fillMaxWidth(),
            )
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            if (state.isSaving) Text("Saving…")
            Button(onClick = onForgetDevice, modifier = Modifier.fillMaxWidth()) {
                Text("Forget device")
            }
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
            onBack = {},
        )
    }
}
