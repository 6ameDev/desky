package com.desky.shared.domain

import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.matcher.matches
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest

private fun testDevice() = PairedDevice(
    id = "desky-$ROBOT_HOST",
    ip = ROBOT_HOST,
    port = DEFAULT_UDP_PORT,
    authToken = "psk-v1",
)

private const val PSK_HEX = "00112233445566778899aabbccddeeff"

private fun psk() = hexToBytes(PSK_HEX)

class SendSignedControlUseCaseTest {

    @Test
    fun sendsExactSignedBytesToDeviceAddress() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit
        val device = testDevice()
        val cmd = ControlCommand(RobotMode.AUTONOMOUS, 0.5f, -0.25f)
        val expected = encodeSigned(cmd, 7u, psk())

        val result = SendSignedControlUseCase(udp)(device, cmd, psk(), 7u)

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend {
            udp.send(matches { actual: ByteArray -> actual.contentEquals(expected) }, device.ip, device.port)
        }
    }

    @Test
    fun counterGoesOnTheWire() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit
        val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f)

        SendSignedControlUseCase(udp)(testDevice(), cmd, psk(), 1u)
        SendSignedControlUseCase(udp)(testDevice(), cmd, psk(), 2u)

        // Counter BE bytes 5..8 of each frame.
        verifySuspend {
            udp.send(
                matches { actual: ByteArray ->
                    actual.size == DRIVE_SIZE &&
                        actual[5] == 0.toByte() && actual[6] == 0.toByte() &&
                        actual[7] == 0.toByte() && actual[8] == 1.toByte()
                },
                any(),
                any(),
            )
        }
    }

    @Test
    fun shortPskRejectedAsFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit

        // Like the legacy sender, every throw (including the codec's
        // require) maps to Outcome.Failure — never an escape.
        val result = SendSignedControlUseCase(udp)(
            testDevice(),
            ControlCommand(RobotMode.MANUAL, 0f, 0f),
            ByteArray(8),
            1u,
        )

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.contains("psk"), "reason: ${failure.reason}")
    }

    @Test
    fun sendThrowingMapsToFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } calls { throw IllegalStateException("no route") }

        val result = SendSignedControlUseCase(udp)(
            testDevice(),
            ControlCommand(RobotMode.MANUAL, 0f, 0f),
            psk(),
            1u,
        )

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }

    @Test
    fun cancellationPropagatesInsteadOfFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } calls { throw CancellationException("cancelled") }

        // Structured concurrency: cancellation must escape, never map to Outcome.Failure.
        assertFailsWith<CancellationException> {
            SendSignedControlUseCase(udp)(
                testDevice(),
                ControlCommand(RobotMode.MANUAL, 0f, 0f),
                psk(),
                1u,
            )
        }
    }
}

class EmergencyStopUseCaseTest {

    @Test
    fun sendsSignedZeroedManualCommandWithEstopFlag() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit
        val device = testDevice()
        val expected = encodeSigned(
            ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG),
            3u,
            psk(),
        )

        val result = EmergencyStopUseCase(SendSignedControlUseCase(udp))(device, psk(), 3u)

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend {
            udp.send(matches { actual: ByteArray -> actual.contentEquals(expected) }, device.ip, device.port)
        }
        // The signed frame carries the flag byte and the frozen golden tag
        // inputs (counter=3 differs from the counter=1 golden vector, so
        // only the layout is pinned here — CodecTest pins the tag bytes).
        val sent = expected
        assertTrue(sent.size == DRIVE_SIZE)
        assertTrue((sent[0].toInt() and 0xFF) == DRIVE_HEADER)
        assertTrue((sent[4].toInt() and 0xFF) == ESTOP_FLAG)
    }

    @Test
    fun estopFlagIsBit0() {
        kotlin.test.assertEquals(0x01, ESTOP_FLAG)
    }

    @Test
    fun sendThrowingMapsToFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } calls { throw IllegalStateException("no route") }

        val result = EmergencyStopUseCase(SendSignedControlUseCase(udp))(testDevice(), psk(), 1u)

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }

    @Test
    fun cancellationPropagatesInsteadOfFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } calls { throw CancellationException("cancelled") }

        // Must escape through both layers (sendSigned + estop), never map to Outcome.Failure.
        assertFailsWith<CancellationException> {
            EmergencyStopUseCase(SendSignedControlUseCase(udp))(testDevice(), psk(), 1u)
        }
    }

    @Test
    fun delegatesToSignedSenderBytes() = runTest {
        // E-stop shares the exact wire path: delegate output must equal a
        // direct signed send of the centered+flag command.
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit
        val device = testDevice()
        val cmd = ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG)

        EmergencyStopUseCase(SendSignedControlUseCase(udp))(device, psk(), 9u)
        val direct = encodeSigned(cmd, 9u, psk())

        verifySuspend {
            udp.send(matches { actual: ByteArray -> actual.contentEquals(direct) }, device.ip, device.port)
        }
        assertContentEquals(direct, encodeSigned(cmd, 9u, psk()))
    }
}
