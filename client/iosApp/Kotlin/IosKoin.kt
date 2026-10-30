package com.desky.shared.di

import com.desky.shared.core.createPairSecretStore
import com.desky.shared.core.createDeskyDatabase
import com.desky.shared.features.control.DrivingViewModel
import com.desky.shared.features.control.ExpressionsViewModel
import com.desky.shared.features.home.HomeViewModel
import com.desky.shared.features.pairing.PairingViewModel
import com.desky.shared.presentation.RobotControlViewModel
import com.desky.shared.features.settings.SettingsViewModel
import org.koin.core.Koin
import org.koin.core.context.startKoin
import platform.Foundation.NSLock

private var iosKoin: Koin? = null

/**
 * Guards [initKoinForIos] and the three ViewModel accessors below.
 *
 * Swift callers are `@MainActor`, but that does not constrain Kotlin/Native
 * threads: Koin callbacks / background workers can enter these accessors off
 * the main thread, so the plain check-then-set on the nullable caches above
 * would race (double `startKoin` / duplicate VM instances + duplicate
 * heartbeat loops). All four accessors serialize on this lock; no API or
 * behavior change.
 *
 * `NSLock` rather than `synchronized`: Kotlin's `synchronized` is JVM-only
 * and does not resolve on Kotlin/Native (verified: `Unresolved reference
 * 'synchronized'` in `:shared:compileKotlinIosSimulatorArm64`).
 */
private val lock = NSLock()

private inline fun <T> guarded(block: () -> T): T {
    lock.lock()
    try {
        return block()
    } finally {
        lock.unlock()
    }
}

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
fun initKoinForIos(): Koin = guarded {
    iosKoin?.let { return@guarded it }
    val koin = startKoin {
        modules(
            dataModule(createDeskyDatabase(), createPairSecretStore()),
            domainModule,
            controlModule,
            homeModule,
            presentationModule,
            pairingModule,
            settingsModule,
            sessionModule,
        )
    }.koin
    iosKoin = koin
    return@guarded koin
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
private var sharedHomeVm: HomeViewModel? = null
private var sharedDrivingVm: DrivingViewModel? = null
private var sharedExpressionsVm: ExpressionsViewModel? = null

fun Koin.pairingViewModel(): PairingViewModel = guarded {
    sharedPairingVm?.let { return@guarded it }
    return@guarded get<PairingViewModel>().also { sharedPairingVm = it }
}

fun Koin.robotControlViewModel(): RobotControlViewModel = guarded {
    sharedControlVm?.let { return@guarded it }
    return@guarded get<RobotControlViewModel>().also { sharedControlVm = it }
}

fun Koin.settingsViewModel(): SettingsViewModel = guarded {
    sharedSettingsVm?.let { return@guarded it }
    return@guarded get<SettingsViewModel>().also { sharedSettingsVm = it }
}

fun Koin.homeViewModel(): HomeViewModel = guarded {
    sharedHomeVm?.let { return@guarded it }
    return@guarded get<HomeViewModel>().also { sharedHomeVm = it }
}

fun Koin.drivingViewModel(): DrivingViewModel = guarded {
    sharedDrivingVm?.let { return@guarded it }
    return@guarded get<DrivingViewModel>().also { sharedDrivingVm = it }
}

fun Koin.expressionsViewModel(): ExpressionsViewModel = guarded {
    sharedExpressionsVm?.let { return@guarded it }
    return@guarded get<ExpressionsViewModel>().also { sharedExpressionsVm = it }
}
