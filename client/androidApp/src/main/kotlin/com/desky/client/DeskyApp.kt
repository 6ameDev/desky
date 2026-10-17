package com.desky.client

import android.app.Application
import com.desky.shared.data.createBondSecretStore
import com.desky.shared.data.createDeskyDatabase
import com.desky.shared.data.createWifiSocketBinder
import com.desky.shared.di.dataModule
import com.desky.shared.di.domainModule
import com.desky.shared.di.presentationModule
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
                    createBondSecretStore(this@DeskyApp),
                    createWifiSocketBinder(this@DeskyApp),
                ),
                domainModule,
                presentationModule,
            )
        }
    }
}
