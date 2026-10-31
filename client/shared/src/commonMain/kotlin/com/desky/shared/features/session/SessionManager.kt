package com.desky.shared.features.session

import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.PairedDevice
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.features.pairing.ForgetDeviceUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

class SessionManager(
    private val deviceRepo: DeviceRepository,
    getPairPsk: GetPairPskUseCase,
    private val toRobotCounter: ToRobotCounter,
    private val forgetUC: ForgetDeviceUseCase,
    private val scope: CoroutineScope,
) {
    private val cache = SessionPskCache(getPairPsk, toRobotCounter)
    private val mutex = Mutex()
    private val _invalid = MutableStateFlow(false)
    val invalid: StateFlow<Boolean> = _invalid.asStateFlow()

    private var currentDevice: PairedDevice? = null
    private var forgetFiredFor: String? = null
    private var sessionScope: CoroutineScope? = null

    init {
        start()
    }

    fun start() {
        if (sessionScope != null) return
        val session = CoroutineScope(scope.coroutineContext + SupervisorJob(scope.coroutineContext[Job]))
        sessionScope = session
        session.launch { collectPaired() }
    }

    fun stop() {
        sessionScope?.cancel()
        sessionScope = null
    }

    suspend fun allocate(dev: PairedDevice): Pair<ByteArray, UInt>? {
        val prepared: Pair<ByteArray, UInt>?
        val shouldForget: Boolean
        mutex.withLock {
            prepared = cache.allocate(dev)
            if (prepared != null) {
                shouldForget = false
            } else {
                if (forgetFiredFor == dev.id) {
                    shouldForget = false
                } else {
                    forgetFiredFor = dev.id
                    shouldForget = true
                }
            }
        }
        if (prepared != null) {
            if (_invalid.value) _invalid.value = false
            return prepared
        }
        _invalid.value = true
        if (shouldForget) {
            val session = sessionScope ?: scope
            session.launch {
                try {
                    forgetUC()
                } catch (e: CancellationException) {
                    throw e
                } catch (_: Exception) {
                }
            }
        }
        return null
    }

    suspend fun invalidate() {
        mutex.withLock { cache.invalidate() }
    }

    private suspend fun collectPaired() {
        deviceRepo.observePaired().collect { paired ->
            if (paired == null) {
                mutex.withLock {
                    cache.invalidate()
                    toRobotCounter.reset()
                    currentDevice = null
                }
            } else {
                val rotated: Boolean
                mutex.withLock {
                    val prev = currentDevice
                    currentDevice = paired
                    rotated = prev == null || prev.id != paired.id || prev.era != paired.era
                    if (rotated) {
                        cache.invalidate()
                        forgetFiredFor = null
                    }
                }
                if (rotated) _invalid.value = false
            }
        }
    }

    companion object {
        const val REPAIR_COPY = "Session invalid — please re-pair"
    }
}
