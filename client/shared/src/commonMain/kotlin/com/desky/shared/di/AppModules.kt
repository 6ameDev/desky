package com.desky.shared.di

import com.desky.shared.data.RoomDeviceRepository
import com.desky.shared.data.RoomSettingsRepository
import com.desky.shared.data.createUdpSocketManager
import com.desky.shared.data.db.DeskyDatabase
import com.desky.shared.domain.BondSecretStore
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.DriveCounter
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.GetBondPskUseCase
import com.desky.shared.domain.ObserveDeviceConfigUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.PairDeviceWithCodeUseCase
import com.desky.shared.domain.AliveUseCase
import com.desky.shared.domain.RekeyUseCase
import com.desky.shared.domain.SaveDeviceConfigUseCase
import com.desky.shared.domain.SendSignedControlUseCase
import com.desky.shared.domain.SettingsRepository
import com.desky.shared.domain.UdpSocketBinder
import com.desky.shared.domain.UdpSocketManager
import com.desky.shared.presentation.PairingViewModel
import com.desky.shared.presentation.RobotControlViewModel
import com.desky.shared.presentation.SettingsViewModel
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import org.koin.core.module.Module
import org.koin.core.module.dsl.viewModel
import org.koin.core.qualifier.named
import org.koin.dsl.module

/**
 * Data-layer module. Takes the platform-built [DeskyDatabase] (Room needs a
 * platform context/builder, so the app module constructs it and passes it
 * in), the platform-built [BondSecretStore] (Android's
 * EncryptedSharedPreferences likewise needs a Context), and the optional
 * platform [UdpSocketBinder] (Android Wi-Fi pinning; null on JVM/iOS), and
 * wires repositories + the UDP transport.
 */
fun dataModule(
    db: DeskyDatabase,
    bondSecretStore: BondSecretStore,
    socketBinder: UdpSocketBinder? = null,
): Module = module {
    single { db }
    single<BondSecretStore> { bondSecretStore }
    single<DeviceRepository> { RoomDeviceRepository(get<DeskyDatabase>().pairedDeviceDao()) }
    single<SettingsRepository> { RoomSettingsRepository(get<DeskyDatabase>().robotConfigDao()) }
    single(named("ioScope")) { CoroutineScope(SupervisorJob() + Dispatchers.Default) }
    single<UdpSocketManager> {
        createUdpSocketManager(get(named("ioScope"))).also { socketBinder?.attach(it) }
    }
    // Expose the platform binder for the control VM's AP-rejoin knock:
    // Android registers the shared WifiSocketBinder; JVM/iOS pass null, so
    // no binding exists there and `getOrNull<UdpSocketBinder>()` stays null.
    socketBinder?.let { binder -> single<UdpSocketBinder> { binder } }
}

/** Domain-layer module: all use cases as factories + the shared drive counter. */
val domainModule: Module = module {
    single { DriveCounter(get()) }
    factory { PairDeviceWithCodeUseCase(get(), get(), get()) }
    factory { GetBondPskUseCase(get(), get()) }
    factory { SendSignedControlUseCase(get()) }
    factory { ObserveTelemetryUseCase(get()) }
    factory { EmergencyStopUseCase(get()) }
    factory { SaveDeviceConfigUseCase(get()) }
    factory { ObserveDeviceConfigUseCase(get()) }
    factory { ForgetDeviceUseCase(get(), get(), get(), get(), get()) }
    factory { RekeyUseCase(get(), get(), get(), get(), get()) }
    factory { AliveUseCase(get()) }
}

/**
 * Presentation-layer module. Koin 4 core `viewModel` DSL (from
 * `koin-core-viewmodel`) — no koin-android needed in :shared.
 */
val presentationModule: Module = module {
    viewModel { PairingViewModel(get(), get(), get()) }
    // rekeyUC/aliveUC/socketBinder are named (tickMs keeps its default):
    // the rekey trigger stays inert when the domain module has no RekeyUseCase
    // binding, the ALIVE/DISCONNECT knock stays inert without an AliveUseCase
    // binding, and the AP-rejoin knock stays inert without a UdpSocketBinder
    // binding (JVM/iOS — getOrNull, tests, legacy graphs).
    viewModel { RobotControlViewModel(get(), get(), get(), get(), get(), get(), get(), rekeyUC = getOrNull<RekeyUseCase>(), aliveUC = getOrNull<AliveUseCase>(), socketBinder = getOrNull<UdpSocketBinder>()) }
    viewModel { SettingsViewModel(get(), get(), get()) }
}
