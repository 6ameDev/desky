package com.desky.shared.presentation

import com.desky.shared.domain.DEFAULT_UDP_PORT
import com.desky.shared.domain.PairSecretStore
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.PairedDevice
import com.desky.shared.domain.RobotConfig
import com.desky.shared.domain.SettingsRepository
import com.desky.shared.domain.UdpDatagram
import com.desky.shared.domain.UdpSocketBinder
import com.desky.shared.domain.UdpSocketManager
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.asSharedFlow
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

/** Hand-written fake: controllable pairing with save/clear/counter counters. */
class FakeDeviceRepository(initial: PairedDevice? = null) : DeviceRepository {
    private val flow = MutableStateFlow(initial)
    var saveCalls = 0
    var clearCalls = 0
    var saveError: Throwable? = null
    var clearError: Throwable? = null
    /** Every (deviceId, counter) persisted via [updateCounter], in order. */
    val counterSaves = mutableListOf<Pair<String, UInt>>()

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

    override suspend fun updateCounter(deviceId: String, counter: UInt) {
        counterSaves += deviceId to counter
        val current = flow.value
        if (current?.id == deviceId) flow.value = current.copy(lastCounter = counter)
    }

    /** Every (deviceId, counter, era) persisted via [updatePairRefresh], in order. */
    val pairRefreshSaves = mutableListOf<Triple<String, UInt, UShort>>()

    override suspend fun updatePairRefresh(deviceId: String, counter: UInt, era: UShort) {
        pairRefreshSaves += Triple(deviceId, counter, era)
        val current = flow.value
        if (current?.id == deviceId) flow.value = current.copy(lastCounter = counter, era = era, lastRobotCounter = 0u)
    }

    /** Every (deviceId, counter) persisted via [updateRobotCounter], in order. */
    val robotCounterSaves = mutableListOf<Pair<String, UInt>>()

    override suspend fun updateRobotCounter(deviceId: String, counter: UInt) {
        robotCounterSaves += deviceId to counter
        val current = flow.value
        if (current?.id == deviceId) flow.value = current.copy(lastRobotCounter = counter)
    }

    fun emit(device: PairedDevice?) {
        flow.value = device
    }
}

/** Hand-written fake vault: in-memory map mirroring [PairSecretStore] semantics. */
class FakePairSecretStore : PairSecretStore {
    private val vault = mutableMapOf<String, String>()
    var saveCalls = 0
    var loadCalls = 0
    var clearCalls = 0

    override suspend fun savePsk(deviceId: String, pskHex: String) {
        saveCalls++
        vault[deviceId] = pskHex
    }

    override suspend fun loadPsk(deviceId: String): String? {
        loadCalls++
        return vault[deviceId]
    }

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
 * Hand-written fake binder: non-suspending rejoin signal mirroring the
 * production `WifiSocketBinder` contract (`MutableSharedFlow(
 * extraBufferCapacity = 1)` + `tryEmit`). `attach`/`detach` are no-ops;
 * tests fire [emitNetworkAvailable] directly (no 1 s debounce here — the
 * debounce lives in the Android binder; the VM collapses bursts
 * idempotently, so a flap still yields one `ALIVE`).
 */
class FakeUdpSocketBinder : UdpSocketBinder {
    private val _networkAvailable = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    override val networkAvailable: SharedFlow<Unit> = _networkAvailable.asSharedFlow()

    override fun attach(manager: UdpSocketManager) = Unit

    override fun detach() = Unit

    /** Non-suspending, like the production `onAvailable` path. */
    fun emitNetworkAvailable() {
        _networkAvailable.tryEmit(Unit)
    }
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
