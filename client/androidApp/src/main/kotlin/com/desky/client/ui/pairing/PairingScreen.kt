package com.desky.client.ui.pairing

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.ROBOT_HOST
import com.desky.shared.presentation.PairingUiState
import com.desky.shared.presentation.PairingViewModel
import org.koin.androidx.compose.koinViewModel

@Composable
fun PairingScreen(
    onPaired: () -> Unit,
    onOpenControls: () -> Unit,
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
        onOpenControls = onOpenControls,
        onOpenScan = onOpenScan,
    )
}

@Composable
fun PairingContent(
    state: PairingUiState,
    onPinChange: (String) -> Unit,
    onPair: () -> Unit,
    onUnpair: () -> Unit,
    onOpenControls: () -> Unit,
    onOpenScan: () -> Unit,
) {
    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("Pair with Desky", style = MaterialTheme.typography.headlineSmall)
            OutlinedTextField(
                value = state.pin,
                onValueChange = onPinChange,
                label = { Text("Pairing code") },
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                modifier = Modifier.fillMaxWidth(),
            )
            Button(
                onClick = onPair,
                // Paired gate (mirrors PairingViewModel.onPair): while a
                // device is paired only Forget is offered, never a re-pair
                // over the live pair.
                enabled = !state.isPairing && state.pairedDevice == null,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    if (state.isPairing) {
                        if (state.pairingAttempt <= 1) "Pairing…" else "Pairing… (attempt ${state.pairingAttempt})"
                    } else "Pair",
                )
            }
            // QR shortcut next to the manual field (manual entry stays
            // forever): decoded digits feed the identical onPair path.
            OutlinedButton(
                onClick = onOpenScan,
                enabled = state.pairedDevice == null,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("Scan QR")
            }
            state.error?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            state.pairedDevice?.let { device ->
                Text("Paired: ${device.alias}")
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = onOpenControls) { Text("Open Controls") }
                    OutlinedButton(onClick = onUnpair) { Text("Unpair") }
                }
            }
        }
    }
}

@Preview
@Composable
private fun PairingContentPreview() {
    MaterialTheme {
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
            onOpenControls = {},
            onOpenScan = {},
        )
    }
}
