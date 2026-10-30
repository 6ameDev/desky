package com.desky.client

import android.app.Application
import com.desky.shared.core.createPairSecretStore
import com.desky.shared.core.createDeskyDatabase
import com.desky.shared.core.createWifiSocketBinder
import com.desky.shared.di.dataModule
import com.desky.shared.di.controlModule
import com.desky.shared.di.domainModule
import com.desky.shared.di.homeModule
import com.desky.shared.di.pairingModule
import com.desky.shared.di.presentationModule
import com.desky.shared.di.sessionModule
import com.desky.shared.di.settingsModule
import org.koin.android.ext.koin.androidContext
import org.koin.core.context.startKoin

/**
 * App scope owns the UDP socket singleton (see `dataModule` +
 * `DatagramSocketManager` KDoc): ViewModels must never close it. No
 * `onTerminate`/`onLowMemory` close wiring by design — `onTerminate` never
 * runs on real devices and the fd dies with the process; adding an observer
 * here would pull a new lifecycle dependency for zero release-build effect.
 */
class DeskyApp : Application() {
    override fun onCreate() {
        super.onCreate()
        startKoin {
            androidContext(this@DeskyApp)
            modules(
                dataModule(
                    createDeskyDatabase(this@DeskyApp),
                    createPairSecretStore(this@DeskyApp),
                    createWifiSocketBinder(this@DeskyApp),
                ),
                domainModule,
                controlModule,
                homeModule,
                presentationModule,
                pairingModule,
                settingsModule,
                sessionModule,
            )
        }
    }
}
