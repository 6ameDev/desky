package com.desky.client

import android.app.Application
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.desky.shared.core.createPairSecretStore
import com.desky.shared.core.createDeskyDatabase
import com.desky.shared.core.createWifiSocketBinder
import com.desky.shared.di.dataModule
import com.desky.shared.features.control.ConnectionManager
import com.desky.shared.features.control.DriveController
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
        val koin = startKoin {
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
        }.koin
        // App-wide link lifeline: background sends DISCONNECT (pair kept) and
        // pauses the drive loop, foreground knocks ALIVE due and resumes it.
        // One observer — screens never own this.
        ProcessLifecycleOwner.get().lifecycle.addObserver(
            object : DefaultLifecycleObserver {
                override fun onStop(owner: LifecycleOwner) {
                    koin.get<ConnectionManager>().onBackground()
                    koin.get<DriveController>().onBackground()
                }

                override fun onStart(owner: LifecycleOwner) {
                    koin.get<ConnectionManager>().onForeground()
                    koin.get<DriveController>().onForeground()
                }
            },
        )
    }
}
