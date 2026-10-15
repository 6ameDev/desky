package com.desky.shared.presentation

import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.BondSecretStore
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.RobotConfig
import com.desky.shared.domain.SettingsRepository
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.UdpSocketManager
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestCoroutineScheduler
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain

fun testDevice(ip: String = "192.168.4.1") = PairedDevice(
    id = "desky-$ip",
    ip = ip,
    port = DEFAULT_UDP_PORT,
    authToken = "token",
)

/** Hand-written fake: controllable pairing with save/clear counters. */
class FakeDeviceRepository(initial: PairedDevice? = null) : DeviceRepository {
    private val flow = MutableStateFlow(initial)
    var saveCalls = 0
    var clearCalls = 0
    var saveError: Throwable? = null
    var clearError: Throwable? = null

    override fun observePaired(): Flow<PairedDevice?> = flow

    override suspend fun save(device: PairedDevice) {
        saveError?.let { throw it }
        saveCalls++
        flow.value = device
    }

    override suspend fun clear() {
        clearError?.let { throw it }
        clearCalls++
        flow.value = null
    }

    override suspend fun clearPskHex(deviceId: String) {
        val current = flow.value
        if (current?.id == deviceId) flow.value = current.copy(pskHex = null)
    }

    fun emit(device: PairedDevice?) {
        flow.value = device
    }
}

/** Hand-written fake vault: in-memory map mirroring [BondSecretStore] semantics. */
class FakeBondSecretStore : BondSecretStore {
    private val vault = mutableMapOf<String, String>()
    var saveCalls = 0
    var clearCalls = 0

    override suspend fun savePsk(deviceId: String, pskHex: String) {
        saveCalls++
        vault[deviceId] = pskHex
    }

    override suspend fun loadPsk(deviceId: String): String? = vault[deviceId]

    override suspend fun clear(deviceId: String) {
        clearCalls++
        vault.remove(deviceId)
    }

    /** Pre-load an entry without counting a [savePsk] call. */
    fun seed(deviceId: String, pskHex: String) {
        vault[deviceId] = pskHex
    }
}

/** Hand-written fake: records every saved config. */
class FakeSettingsRepository(initial: RobotConfig = RobotConfig()) : SettingsRepository {
    private val flow = MutableStateFlow(initial)
    val saved = mutableListOf<RobotConfig>()
    var saveError: Throwable? = null

    /**
     * Optional gate: when set, [save] parks on it, so tests can observe the
     * in-flight ([SettingsUiState.isSaving]) window deterministically.
     */
    var saveGate: CompletableDeferred<Unit>? = null

    override fun observeConfig(): Flow<RobotConfig> = flow

    override suspend fun save(config: RobotConfig) {
        saveGate?.await()
        saveError?.let { throw it }
        saved += config
        flow.value = config
    }
}

/** Hand-written fake socket: scripted inbound flow plus a send record. */
class FakeUdpSocketManager : UdpSocketManager {
    /**
     * Buffered like the real socket contract (see `createUdpSocketManager`:
     * replay 0, 64-slot buffer), so test-side `emit` never suspends waiting
     * for the VM's collector to be pumped.
     */
    val inbound = MutableSharedFlow<UdpDatagram>(extraBufferCapacity = 64)
    override val incoming: Flow<UdpDatagram> = inbound
    val sent = mutableListOf<UdpDatagram>()
    var sendError: Throwable? = null

    override suspend fun send(bytes: ByteArray, host: String, port: Int) {
        sendError?.let { throw it }
        sent += UdpDatagram(bytes, host, port)
    }

    override suspend fun close() = Unit
}

/**
 * Runs [block] with the JVM `Main` dispatcher installed (it is absent on
 * plain JVM unit tests, but `viewModelScope` needs `Main.immediate`).
 *
 * Main runs on its OWN [TestCoroutineScheduler] (`main`), deliberately NOT
 * shared with the `runTest` scheduler: VMs own infinite loops (the control
 * heartbeat, repo collectors) that would make `runTest`'s end-of-test
 * `advanceUntilIdle` flush — and any test-side `advanceUntilIdle` — hang
 * forever. Pump the VM explicitly with `main.runCurrent()` /
 * `main.advanceTimeBy(...)`; never `advanceUntilIdle()` while an infinite
 * loop is alive.
 */
@OptIn(ExperimentalCoroutinesApi::class)
fun runVmTest(block: suspend TestScope.(main: TestCoroutineScheduler) -> Unit) = runTest {
    val main = TestCoroutineScheduler()
    Dispatchers.setMain(StandardTestDispatcher(main))
    try {
        block(main)
    } finally {
        Dispatchers.resetMain()
    }
}
