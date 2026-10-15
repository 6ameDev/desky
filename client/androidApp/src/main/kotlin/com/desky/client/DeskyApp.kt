package com.desky.client

import android.app.Application
import com.desky.shared.data.createBondSecretStore
import com.desky.shared.data.createDeskyDatabase
import com.desky.shared.di.dataModule
import com.desky.shared.di.domainModule
import com.desky.shared.di.presentationModule
import org.koin.android.ext.koin.androidContext
import org.koin.core.context.startKoin

class DeskyApp : Application() {
    override fun onCreate() {
        super.onCreate()
        startKoin {
            androidContext(this@DeskyApp)
            modules(
                dataModule(createDeskyDatabase(this@DeskyApp), createBondSecretStore(this@DeskyApp)),
                domainModule,
                presentationModule,
            )
        }
    }
}
