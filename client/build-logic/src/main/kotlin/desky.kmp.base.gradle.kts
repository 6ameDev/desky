// AGP 9+: KMP modules use com.android.kotlin.multiplatform.library —
// com.android.library + kotlin-multiplatform together is rejected when
// built-in Kotlin is enabled (the default).
plugins {
    id("org.jetbrains.kotlin.multiplatform")
    id("com.android.kotlin.multiplatform.library")
}

kotlin {
    // AGP 9 KMP: android config lives in kotlin.android (namespace,
    // compileSdk, minSdk flat — no defaultConfig wrapper). The old top-level
    // android {} block is gone with com.android.library.
    android {
        namespace = "com.desky.shared"
        compileSdk = 36
        minSdk = 26

        compilerOptions {
            jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        }
    }

    jvm {
        compilerOptions {
            jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        }
    }

    // NOTE: no iosX64 — androidx.room3:room3-runtime and androidx.sqlite:sqlite-bundled
    // (mandatory pins, commonMain deps) publish iosArm64 + iosSimulatorArm64 but no
    // Intel-simulator variant, so an iosX64 target cannot resolve commonMain deps.
    // iosX64 is also unrunnable on Apple-silicon hosts and deprecated upstream.
    iosArm64()
    iosSimulatorArm64()
}
