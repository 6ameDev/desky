package com.desky.shared.presentation

import app.cash.turbine.test
import com.desky.shared.domain.DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS
import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.DriveCounter
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.GetPairPskUseCase
import com.desky.shared.domain.PairDeviceWithCodeUseCase
import com.desky.shared.domain.PAIR_ERR_HEADER
import com.desky.shared.domain.PAIR_OK_HEADER
import com.desky.shared.domain.ROBOT_HOST
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.aesGcmEncrypt
import com.desky.shared.domain.hkdfSha256
import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.every
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.mock
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.flowOf
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertIs
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

private fun hex(s: String): ByteArray {
    require(s.length % 2 == 0)
    return ByteArray(s.length / 2) { i ->
        s.substring(i * 2, i * 2 + 2).toInt(16).toByte()
    }
}

/** `PAIR_OK` with REAL crypto for the nonce the VM just sent. */
private suspend fun pairOkFor(clientNonce: ByteArray): ByteArray {
    val serverNonce = hex("05060708")
    val psk = hex("00112233445566778899aabbccddeeff")
    val code = "123456".encodeToByteArray()
    val kek = hkdfSha256(clientNonce + serverNonce, code, "desky-pair-v1", 16)
    val nonce12 = serverNonce + clientNonce + ByteArray(4)
    val aad = byteArrayOf(PAIR_OK_HEADER.toByte()) + clientNonce + serverNonce
    val (ct, tag) = aesGcmEncrypt(kek, nonce12, aad, psk)
    val body = byteArrayOf(PAIR_OK_HEADER.toByte()) + serverNonce + ct + tag
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and 0xFF)
    return body + byteArrayOf(xor.toByte())
}

private fun pairErr(reason: Int, retryS: Int): ByteArray {
    val body = byteArrayOf(PAIR_ERR_HEADER.toByte(), reason.toByte(), retryS.toByte())
    var xor = 0
    for (b in body) xor = xor xor (b.toInt() and 0xFF)
    return body + byteArrayOf(xor.toByte())
}

@OptIn(ExperimentalCoroutinesApi::class)
class PairingViewModelTest {

    private fun vm(
        repo: DeviceRepository,
        udp: FakeUdpSocketManager = FakeUdpSocketManager(),
        vault: FakePairSecretStore = FakePairSecretStore(),
        attemptTimeoutsMs: List<Long> = DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS,
    ) = PairingViewModel(
        PairDeviceWithCodeUseCase(repo, udp, vault, attemptTimeoutsMs),
        ForgetDeviceUseCase(repo, vault, udp, GetPairPskUseCase(vault, repo), DriveCounter(repo)),
        repo,
    )

    /** Pumps until the VM's initial PAIR_REQ is on the wire; returns its client nonce. */
    private fun clientNonceOf(udp: FakeUdpSocketManager): ByteArray {
        assertEquals(1, udp.sent.size, "exactly one PAIR_REQ before any reply")
        return udp.sent[0].bytes.copyOfRange(7, 11)
    }

    @Test
    fun pinIntentUpdatesState() = runVmTest { main ->
        val viewModel = vm(FakeDeviceRepository())
        viewModel.state.test {
            assertEquals(PairingUiState(), awaitItem())
            viewModel.onPinChange("123456")
            assertEquals("123456", awaitItem().pin)
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun pairSuccessStoresDevice() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val udp = FakeUdpSocketManager()
        val vault = FakePairSecretStore()
        val viewModel = vm(repo, udp, vault)
        main.runCurrent()

        viewModel.onPinChange("123456")
        viewModel.onPair()
        assertTrue(viewModel.state.value.isPairing)
        main.runCurrent()
        val clientNonce = clientNonceOf(udp)
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.error)
        assertEquals(1, state.pairingAttempt)
        val device = assertNotNull(state.pairedDevice)
        assertEquals("192.168.4.1", device.ip)
        assertEquals("psk-v1", device.authToken)
        // Room row carries no key material and resets the drive counter;
        // the vault holds the pair PSK.
        assertEquals(0u, device.lastCounter)
        assertEquals("00112233445566778899aabbccddeeff", vault.loadPsk(device.id))
        assertEquals(1, repo.saveCalls)
    }

    @Test
    fun pairFailureMapsReasonToError() = runVmTest { main ->
        val viewModel = vm(FakeDeviceRepository())
        main.runCurrent()

        viewModel.onPinChange("12")
        viewModel.onPair()
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.pairedDevice)
        val error = assertNotNull(state.error)
        assertTrue(error.isNotBlank())
    }

    @Test
    fun pairErrSurfacesRobotMessage() = runVmTest { main ->
        val udp = FakeUdpSocketManager()
        val viewModel = vm(FakeDeviceRepository(), udp)
        main.runCurrent()

        viewModel.onPinChange("123456")
        viewModel.onPair()
        main.runCurrent()
        assertEquals(1, udp.sent.size, "exactly one PAIR_REQ before any reply")
        // The VM's collector is active by now, so the queued reply lands.
        udp.inbound.emit(UdpDatagram(pairErr(0, 0), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.pairedDevice)
        assertTrue(assertNotNull(state.error).contains("incorrect code"))
    }

    @Test
    fun pairGuardsReentryWhileInFlight() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val udp = FakeUdpSocketManager()
        val viewModel = vm(repo, udp)
        main.runCurrent()

        viewModel.onPinChange("123456")
        // Both taps land before the scheduler runs the first pairing: the
        // synchronous isPairing flag makes the second a no-op.
        viewModel.onPair()
        viewModel.onPair()
        main.runCurrent()
        val clientNonce = clientNonceOf(udp)
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.advanceUntilIdle()

        assertEquals(1, repo.saveCalls)
        assertFalse(viewModel.state.value.isPairing)
        assertNotNull(viewModel.state.value.pairedDevice)
    }

    @Test
    fun pairBlockedWhilePairedKeepsForgetEnabled() = runVmTest { main ->
        // Paired gate: Pair is disabled while a device is paired (only
        // Forget + auto-retry offered) — no PAIR_REQ leaves the radio.
        val repo = FakeDeviceRepository(testDevice())
        val udp = FakeUdpSocketManager()
        val viewModel = vm(repo, udp)
        main.runCurrent()
        assertNotNull(viewModel.state.value.pairedDevice)

        viewModel.onPinChange("123456")
        viewModel.onPair()
        main.runCurrent()

        val state = viewModel.state.value
        assertFalse(state.isPairing, "blocked pair never enters the pairing state")
        assertTrue(udp.sent.isEmpty(), "no PAIR_REQ over a live pair")
        assertNull(state.error)
        assertNotNull(state.pairedDevice)

        // Forget stays enabled always: unpair still clears.
        viewModel.onUnpair()
        main.advanceUntilIdle()
        assertNull(viewModel.state.value.pairedDevice)
        assertEquals(1, repo.clearCalls)
    }

    @Test
    fun persistFailureSurfacesError() = runVmTest { main ->
        // Mokkery is genuinely simpler here: one throwing stub without a
        // dedicated fake subclass.
        val repo = mock<DeviceRepository>()
        every { repo.observePaired() } returns flowOf(null)
        everySuspend { repo.save(any()) } calls { throw IllegalStateException("disk full") }
        everySuspend { repo.clear() } calls { /* no-op */ }
        val udp = FakeUdpSocketManager()

        val viewModel = vm(repo, udp)
        main.runCurrent()
        viewModel.onPinChange("123456")
        viewModel.onPair()
        main.runCurrent()
        val clientNonce = clientNonceOf(udp)
        udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertFalse(state.isPairing)
        assertNull(state.pairedDevice)
        assertTrue(state.error!!.contains("disk full"))
    }

    @Test
    fun unpairClearsDevice() = runVmTest { main ->
        val repo = FakeDeviceRepository(testDevice())
        val viewModel = vm(repo)
        main.runCurrent()
        assertNotNull(viewModel.state.value.pairedDevice)

        viewModel.onUnpair()
        main.advanceUntilIdle()

        val state = viewModel.state.value
        assertNull(state.pairedDevice)
        assertNull(state.error)
        assertEquals(1, state.pairingAttempt)
        assertEquals(1, repo.clearCalls)
    }

    @Test
    fun unpairFailureSurfacesError() = runVmTest { main ->
        val repo = FakeDeviceRepository(testDevice()).apply {
            clearError = IllegalStateException("db locked")
        }
        val viewModel = vm(repo)
        main.runCurrent()

        viewModel.onUnpair()
        main.advanceUntilIdle()

        assertNotNull(viewModel.state.value.pairedDevice)
        assertIs<String>(viewModel.state.value.error)
    }

    @Test
    fun repoEmissionUpdatesPairedDevice() = runVmTest { main ->
        val repo = FakeDeviceRepository()
        val viewModel = vm(repo)
        main.runCurrent()
        assertNull(viewModel.state.value.pairedDevice)

        repo.emit(testDevice("10.0.0.9"))
        main.runCurrent()

        assertEquals("10.0.0.9", viewModel.state.value.pairedDevice?.ip)
    }

    @Test
    fun silentPairingWalksAttemptsThenResets() = runVmTest { main ->
        val udp = FakeUdpSocketManager()
        val viewModel = vm(FakeDeviceRepository(), udp, attemptTimeoutsMs = listOf(50L, 100L, 150L))
        main.runCurrent()
        viewModel.onPinChange("123456")

        viewModel.state.test {
            assertEquals(PairingUiState(pin = "123456"), awaitItem())
            viewModel.onPair()
            var s = awaitItem()
            assertTrue(s.isPairing)
            assertEquals(1, s.pairingAttempt)
            main.runCurrent()
            assertEquals(1, udp.sent.size)

            main.advanceTimeBy(50)
            main.runCurrent()
            s = awaitItem()
            assertTrue(s.isPairing)
            assertEquals(2, s.pairingAttempt)

            main.advanceTimeBy(100)
            main.runCurrent()
            s = awaitItem()
            assertTrue(s.isPairing)
            assertEquals(3, s.pairingAttempt)

            main.advanceTimeBy(150)
            main.runCurrent()
            s = awaitItem()
            assertFalse(s.isPairing)
            assertEquals(1, s.pairingAttempt)
            assertNotNull(s.error)
            assertEquals(3, udp.sent.size)
            cancelAndIgnoreRemainingEvents()
        }
    }

    @Test
    fun successResetsPairingAttempt() = runVmTest { main ->
        val udp = FakeUdpSocketManager()
        val viewModel = vm(FakeDeviceRepository(), udp)
        main.runCurrent()
        viewModel.onPinChange("123456")

        viewModel.state.test {
            assertEquals(PairingUiState(pin = "123456"), awaitItem())
            viewModel.onPair()
            var s = awaitItem()
            assertTrue(s.isPairing)
            assertEquals(1, s.pairingAttempt)
            main.runCurrent()
            val clientNonce = clientNonceOf(udp)
            udp.inbound.emit(UdpDatagram(pairOkFor(clientNonce), ROBOT_HOST, DEFAULT_UDP_PORT))
            // The reply resumes the pairing coroutine via a Main dispatch.
            main.runCurrent()
            s = awaitItem()
            assertFalse(s.isPairing)
            assertEquals(1, s.pairingAttempt)
            assertNull(s.error)
            assertNotNull(s.pairedDevice)
            cancelAndIgnoreRemainingEvents()
        }
    }
}
