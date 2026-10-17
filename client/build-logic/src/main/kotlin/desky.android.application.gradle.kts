// AGP 9+: built-in Kotlin (no kotlin-android plugin — AGP forbids it).
// The `kotlin { compilerOptions }` block below is provided by AGP.
plugins {
    id("com.android.application")
}

android {
    namespace = "com.desky.client"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.desky.client"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}
