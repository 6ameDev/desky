package com.desky.shared.di

import com.desky.shared.core.DeviceRepository
import com.desky.shared.core.FromRobotCounter
import com.desky.shared.core.PairSecretStore
import com.desky.shared.core.RoomDeviceRepository
import com.desky.shared.core.RoomSettingsRepository
import com.desky.shared.core.SettingsRepository
import com.desky.shared.core.ToRobotCounter
import com.desky.shared.core.UdpSocketBinder
import com.desky.shared.core.UdpSocketManager
import com.desky.shared.core.createUdpSocketManager
import com.desky.shared.core.db.DeskyDatabase
import com.desky.shared.features.control.AliveUseCase
import com.desky.shared.features.control.ConnectionManager
import com.desky.shared.features.control.DriveController
import com.desky.shared.features.control.DrivingViewModel
import com.desky.shared.features.control.EmergencyStopUseCase
import com.desky.shared.features.control.ExpressionsViewModel
import com.desky.shared.features.control.ObserveRemoteDisconnectUseCase
import com.desky.shared.features.control.ObserveTelemetryUseCase
import com.desky.shared.features.control.PairRefreshUseCase
import com.desky.shared.features.control.ReleaseManualUseCase
import com.desky.shared.features.control.SendSignedControlUseCase
import com.desky.shared.features.control.TakeManualUseCase
import com.desky.shared.features.control.TelemetryHub
import com.desky.shared.features.control.TriggerExpressionUseCase
import com.desky.shared.features.session.SessionManager
import com.desky.shared.features.home.HomeViewModel
import com.desky.shared.features.pairing.ForgetDeviceUseCase
import com.desky.shared.features.pairing.GetPairPskUseCase
import com.desky.shared.features.pairing.PairDeviceWithCodeUseCase
import com.desky.shared.features.pairing.PairingViewModel
import com.desky.shared.features.session.SessionGateViewModel
import com.desky.shared.features.settings.ObserveDeviceConfigUseCase
import com.desky.shared.features.settings.SaveDeviceConfigUseCase
import com.desky.shared.features.settings.SettingsViewModel
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import org.koin.core.module.Module
import org.koin.core.module.dsl.viewModel
import org.koin.core.qualifier.named
import org.koin.dsl.module

/**
 * Data-layer module. Takes the platform-built [DeskyDatabase] (Room needs a
 * platform context/builder, so the app module constructs it and passes it
 * in), the platform-built [PairSecretStore] (Android's
 * EncryptedSharedPreferences likewise needs a Context), and the optional
 * platform [UdpSocketBinder] (Android Wi-Fi pinning; null on JVM/iOS), and
 * wires repositories + the UDP transport.
 */
fun dataModule(
    db: DeskyDatabase,
    bondSecretStore: PairSecretStore,
    socketBinder: UdpSocketBinder? = null,
): Module = module {
    single { db }
    single<PairSecretStore> { bondSecretStore }
    single<DeviceRepository> { RoomDeviceRepository(get<DeskyDatabase>().pairedDeviceDao()) }
    single<SettingsRepository> { RoomSettingsRepository(get<DeskyDatabase>().robotConfigDao()) }
    single(named("ioScope")) { CoroutineScope(SupervisorJob() + Dispatchers.Default) }
    single(named("sessionScope")) { CoroutineScope(SupervisorJob() + sessionDispatcher()) }
    single<UdpSocketManager> {
        createUdpSocketManager(get(named("ioScope"))).also { socketBinder?.attach(it) }
    }
    // Expose the platform binder for the control VM's rejoin knock:
    // Android registers the shared WifiSocketBinder; JVM/iOS pass null, so
    // no binding exists there and `getOrNull<UdpSocketBinder>()` stays null.
    socketBinder?.let { binder -> single<UdpSocketBinder> { binder } }
}

/** Kernel module: the shared counter holders. All use cases live in feature modules. */
val domainModule: Module = module {
    single { ToRobotCounter(get()) }
    single { FromRobotCounter(get()) }
}

/** Feature slice modules: canonical homes for pairing, settings, and session bindings. */
val pairingModule: Module = module {
    factory { PairDeviceWithCodeUseCase(get(), get(), get(), get()) }
    factory { GetPairPskUseCase(get(), get()) }
    factory { ForgetDeviceUseCase(get(), get(), get(), get(), get(), get()) }
    viewModel { PairingViewModel(get(), get(), get()) }
}

val settingsModule: Module = module {
    factory { SaveDeviceConfigUseCase(get()) }
    factory { ObserveDeviceConfigUseCase(get()) }
    viewModel { SettingsViewModel(get(), get(), get()) }
}

val sessionModule: Module = module {
    viewModel { SessionGateViewModel(get()) }
    single { SessionManager(get(), get(), get(), get(), get(named("sessionScope"))) }
}

/** Session-scope holders plus the screens bound to them. */
val controlModule: Module = module {
    single {
        ConnectionManager(
            deviceRepo = get(),
            sessionManager = get(),
            observeTelemetry = get(),
            toRobotCounter = get(),
            aliveUC = get(),
            remoteDisconnectUC = get(),
            udp = get(),
            scope = get(named("sessionScope")),
            pairRefreshUC = getOrNull(),
        )
    }
    single { TelemetryHub(udp = get(), scope = get(named("ioScope"))) }
    single {
        DriveController(
            deviceRepo = get(),
            sessionManager = get(),
            sendUC = get(),
            takeManualUC = get(),
            releaseManualUC = get(),
            scope = get(named("sessionScope")),
        )
    }
    viewModel { DrivingViewModel(get(), get(), get(), get(), get()) }
    viewModel { ExpressionsViewModel(get(), get(), getOrNull(), get(), get(), get(), get()) }
    factory { SendSignedControlUseCase(get()) }
    factory { ObserveTelemetryUseCase(get()) }
    factory { EmergencyStopUseCase(get()) }
    factory { PairRefreshUseCase(get(), get(), get(), get(), get(), get()) }
    factory { AliveUseCase(get()) }
    factory { TakeManualUseCase(get()) }
    factory { ReleaseManualUseCase(get()) }
    factory { TriggerExpressionUseCase(get()) }
    factory { ObserveRemoteDisconnectUseCase(get(), get(), get(), get(), get(), get()) }
}

/** Post-pairing landing. */
val homeModule: Module = module {
    viewModel { HomeViewModel(get(), get()) }
}

/** Single lane for session tick loops; sockets stay on the shared pool. */
@OptIn(ExperimentalCoroutinesApi::class)
fun sessionDispatcher(): CoroutineDispatcher = Dispatchers.Default.limitedParallelism(1)
