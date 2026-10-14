package com.desky.shared.di

import com.desky.shared.data.createDeskyDatabase
import com.desky.shared.presentation.PairingViewModel
import com.desky.shared.presentation.RobotControlViewModel
import com.desky.shared.presentation.SettingsViewModel
import org.koin.core.Koin
import org.koin.core.context.startKoin

private var iosKoin: Koin? = null

/**
 * iOS entry point (T8). Starts Koin exactly once and returns the [Koin]
 * instance, wiring the same graph as Android's `DeskyApp`
 * (`dataModule` + `domainModule` + `presentationModule`) backed by the iOS
 * Room database actual ([createDeskyDatabase]).
 *
 * Why this file exists: Koin 4's bootstrap (`KoinApplication` /
 * `startKoin` DSL) and its reified `get()` are not exported to Swift (verified
 * against the generated `shared.framework/Headers/shared.h` — no
 * `KoinApplication`, and `Koin.get` needs a `KClass` token Swift cannot
 * mint). Swift therefore calls this entry point plus the typed accessors
 * below instead of driving Koin directly.
 *
 * Location note: this file lives under `client/iosApp/Kotlin/` and is wired
 * into `:shared`'s `iosMain` via `srcDir` in `shared/build.gradle.kts`, so
 * it ships inside the `shared` framework/pod. `client/shared/src` is frozen
 * for other tasks; nothing under it was touched.
 */
fun initKoinForIos(): Koin {
    iosKoin?.let { return it }
    val koin = startKoin {
        modules(
            dataModule(createDeskyDatabase()),
            domainModule,
            presentationModule,
        )
    }.koin
    iosKoin = koin
    return koin
}

/**
 * Typed ViewModel accessors for Swift. Koin's inline reified `get()` has no
 * Objective-C representation, so Swift resolves the shared singletons
 * through these.
 *
 * Caching is load-bearing, not convenience: `presentationModule` declares
 * the VMs with Koin 4's `viewModel { }` (factory semantics — every `get()`
 * mints a NEW instance; verified on JVM 2026-10-14: two `get()` calls return
 * distinct objects). Minting per view would fork state AND start one command
 * heartbeat loop per `RobotControlViewModel` instance. These accessors
 * resolve once and hand out the same app-lifetime instances that every Swift
 * view observes — the closest SwiftUI analogue of Android's per-destination
 * `koinViewModel()` owners.
 */
private var sharedPairingVm: PairingViewModel? = null
private var sharedControlVm: RobotControlViewModel? = null
private var sharedSettingsVm: SettingsViewModel? = null

fun Koin.pairingViewModel(): PairingViewModel {
    sharedPairingVm?.let { return it }
    return get<PairingViewModel>().also { sharedPairingVm = it }
}

fun Koin.robotControlViewModel(): RobotControlViewModel {
    sharedControlVm?.let { return it }
    return get<RobotControlViewModel>().also { sharedControlVm = it }
}

fun Koin.settingsViewModel(): SettingsViewModel {
    sharedSettingsVm?.let { return it }
    return get<SettingsViewModel>().also { sharedSettingsVm = it }
}
