package com.desky.shared.domain

import dev.mokkery.answering.calls
import dev.mokkery.answering.returns
import dev.mokkery.everySuspend
import dev.mokkery.matcher.any
import dev.mokkery.matcher.matches
import dev.mokkery.mock
import dev.mokkery.verifySuspend
import kotlin.test.Test
import kotlin.test.assertIs
import kotlin.test.assertTrue
import kotlinx.coroutines.test.runTest

private fun testDevice() = PairedDevice(
    id = "desky-192.168.4.1",
    ip = "192.168.4.1",
    port = DEFAULT_UDP_PORT,
    authToken = "token",
)

class SendRobotControlCommandUseCaseTest {

    @Test
    fun sendsExactCodecBytesToDeviceAddress() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit
        val device = testDevice()
        val cmd = ControlCommand(RobotMode.AUTONOMOUS, 0.5f, -0.25f)
        val expected = ControlPacketCodec.encode(cmd)

        val result = SendRobotControlCommandUseCase(udp)(device, cmd)

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend {
            udp.send(matches { actual: ByteArray -> actual.contentEquals(expected) }, device.ip, device.port)
        }
    }

    @Test
    fun sendThrowingMapsToFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } calls { throw IllegalStateException("no route") }

        val result = SendRobotControlCommandUseCase(udp)(testDevice(), ControlCommand(RobotMode.MANUAL, 0f, 0f))

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }
}

class EmergencyStopUseCaseTest {

    @Test
    fun sendsZeroedManualCommandWithEstopFlag() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } returns Unit
        val device = testDevice()
        val expected = ControlPacketCodec.encode(ControlCommand(RobotMode.MANUAL, 0f, 0f, ESTOP_FLAG))

        val result = EmergencyStopUseCase(udp)(device)

        assertIs<Outcome.Success<Unit>>(result)
        verifySuspend {
            udp.send(matches { actual: ByteArray -> actual.contentEquals(expected) }, device.ip, device.port)
        }
    }

    @Test
    fun estopFlagIsBit0() {
        kotlin.test.assertEquals(0x01, ESTOP_FLAG)
    }

    @Test
    fun sendThrowingMapsToFailure() = runTest {
        val udp = mock<UdpSocketManager>()
        everySuspend { udp.send(any(), any(), any()) } calls { throw IllegalStateException("no route") }

        val result = EmergencyStopUseCase(udp)(testDevice())

        val failure = assertIs<Outcome.Failure>(result)
        assertTrue(failure.reason.isNotBlank())
        assertIs<IllegalStateException>(failure.cause)
    }
}
