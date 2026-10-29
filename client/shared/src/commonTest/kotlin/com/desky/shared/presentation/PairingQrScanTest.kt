package com.desky.shared.presentation

import com.desky.shared.domain.DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.PAIR_CODE_SIZE
import com.desky.shared.domain.PAIR_NONCE_SIZE
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.FromRobotCounter
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.ToRobotCounter
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.PairDeviceWithCodeUseCase
import com.desky.shared.domain.PAIR_OK_HEADER
import com.desky.shared.domain.ROBOT_HOST
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.aesGcmEncrypt
import com.desky.shared.domain.hkdfSha256
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

private const val HEX_RADIX = 16
private const val KEK_BYTE_LENGTH = 16
private const val NONCE12_ZERO_PAD_BYTES = 4
private const val BYTE_MASK = 0xFF

private fun qrHex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(HEX_RADIX).toByte()
    }
}

/** `PAIR_OK` with REAL crypto for the nonce the VM just sent (code 123456). */
private suspend fun qrPairOkFor(clientNonce: ByteArray): ByteArray {
    val serverNonce = qrHex("05060708")
    val psk = qrHex("00112233445566778899aabbccddeeff")
    val code = "123456".encodeToByteArray()
    val kek = hkdfSha256(clientNonce + serverNonce, code, "desky-pair-v1", KEK_BYTE_LENGTH)
    val nonce12 = serverNonce + clientNonce + ByteArray(NONCE12_ZERO_PAD_BYTES)
    val aad = byteArrayOf(PAIR_OK_HEADER.toByte()) + clientNonce + serverNonce
    val (ct, tag) = aesGcmEncrypt(kek, nonce12, aad, psk)
    val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + serverNonce + ct + tag
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and BYTE_MASK)
    return body + byteArrayOf(xor.toByte())
}

@OptIn(ExperimentalCoroutinesApi::class)
class PairingQrScanTest {

    private fun vm(
        repo: DeviceRepository,
        udp: FakeUdpSocketManager = FakeUdpSocketManager(),
        vault: FakePairSecretStore = FakePairSecretStore(),
    ) = PairingViewModel(
        PairDeviceWithCodeUseCase(repo, udp, vault, FromRobotCounter(repo), DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS),
        ForgetDeviceUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), ToRobotCounter(repo), FromRobotCounter(repo)),
        repo,
    )

    @Test
    fun validScanStartsPairingWithParsedPin() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val udp = FakeUdpSocketManager()
        val viewModel = vm(repo, udp)
        main.runCurrent()

        viewModel.onQrScanned("123456")
        assertEquals("123456", viewModel.state.value.pin)
        assertTrue(viewModel.state.value.isPairing)
        main.runCurrent()
        assertEquals(1, udp.sent.size, "valid scan fires exactly one PAIR_REQ")

        val clientNonce = udp.sent[0].bytes.copyOfRange(1 + PAIR_CODE_SIZE, 1 + PAIR_CODE_SIZE + PAIR_NONCE_SIZE)
        udp.inbound.emit(UdpDatagram(qrPairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.error)
        assertNotNull(state.pairedDevice)
        assertEquals(1, repo.saveCalls)
    }

    @Test
    fun whitespacePaddedScanTrimsAndPairs() = runVmTest { main ->
        val udp = FakeUdpSocketManager()
        val viewModel = vm(FakeDeviceRepository(), udp)
        main.runCurrent()

        viewModel.onQrScanned("  123456  ")
        assertEquals("123456", viewModel.state.value.pin)
        assertTrue(viewModel.state.value.isPairing)
    }

    @Test
    fun invalidScanSetsErrorWithoutPairing() = runVmTest { main ->
        val udp = FakeUdpSocketManager()
        val viewModel = vm(FakeDeviceRepository(), udp)
        main.runCurrent()

        viewModel.onQrScanned("https://desky.example/pair/123456")
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.pairedDevice)
        assertEquals("QR isn't a Desky code — try again", state.error)
        assertTrue(udp.sent.isEmpty(), "invalid scan sends no PAIR_REQ")
    }

    @Test
    fun duplicateScanWhilePairingIsSingleAttempt() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val udp = FakeUdpSocketManager()
        val viewModel = vm(repo, udp)
        main.runCurrent()

        viewModel.onQrScanned("123456")
        // Duplicate frame while the first pairing is in flight: no-op.
        viewModel.onQrScanned("123456")
        viewModel.onQrScanned("  123456 ")
        main.runCurrent()
        assertEquals(1, udp.sent.size, "duplicate scans while pairing send once")

        val clientNonce = udp.sent[0].bytes.copyOfRange(1 + PAIR_CODE_SIZE, 1 + PAIR_CODE_SIZE + PAIR_NONCE_SIZE)
        udp.inbound.emit(UdpDatagram(qrPairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.advanceUntilIdle()

        assertEquals(1, repo.saveCalls)
        assertFalse(viewModel.state.value.isPairing)
        assertNotNull(viewModel.state.value.pairedDevice)
    }
}
