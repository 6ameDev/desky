package com.desky.shared.di

import com.desky.shared.data.RoomDeviceRepository
import com.desky.shared.data.RoomSettingsRepository
import com.desky.shared.data.createUdpSocketManager
import com.desky.shared.data.db.DeskyDatabase
import com.desky.shared.domain.DeviceRepository
import com.desky.shared.domain.EmergencyStopUseCase
import com.desky.shared.domain.ForgetDeviceUseCase
import com.desky.shared.domain.ObserveDeviceConfigUseCase
import com.desky.shared.domain.ObserveTelemetryUseCase
import com.desky.shared.domain.PairDeviceWithCodeUseCase
import com.desky.shared.domain.SaveDeviceConfigUseCase
import com.desky.shared.domain.SendRobotControlCommandUseCase
import com.desky.shared.domain.SettingsRepository
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
 * in) and wires repositories + the UDP transport.
 */
fun dataModule(db: DeskyDatabase): Module = module {
    single { db }
    single<DeviceRepository> { RoomDeviceRepository(get<DeskyDatabase>().pairedDeviceDao()) }
    single<SettingsRepository> { RoomSettingsRepository(get<DeskyDatabase>().robotConfigDao()) }
    single(named("ioScope")) { CoroutineScope(SupervisorJob() + Dispatchers.Default) }
    single<UdpSocketManager> { createUdpSocketManager(get(named("ioScope"))) }
}

/** Domain-layer module: all 7 use cases as factories. */
val domainModule: Module = module {
    factory { PairDeviceWithCodeUseCase(get(), get()) }
    factory { SendRobotControlCommandUseCase(get()) }
    factory { ObserveTelemetryUseCase(get()) }
    factory { EmergencyStopUseCase(get()) }
    factory { SaveDeviceConfigUseCase(get()) }
    factory { ObserveDeviceConfigUseCase(get()) }
    factory { ForgetDeviceUseCase(get()) }
}

/**
 * Presentation-layer module. Koin 4 core `viewModel` DSL (from
 * `koin-core-viewmodel`) — no koin-android needed in :shared.
 */
val presentationModule: Module = module {
    viewModel { PairingViewModel(get(), get(), get()) }
    viewModel { RobotControlViewModel(get(), get(), get(), get(), get()) }
    viewModel { SettingsViewModel(get(), get(), get()) }
}
