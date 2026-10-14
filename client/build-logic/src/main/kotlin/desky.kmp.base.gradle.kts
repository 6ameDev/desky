plugins {
    id("org.jetbrains.kotlin.multiplatform")
    id("com.android.library")
}

kotlin {
    androidTarget {
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

android {
    namespace = "com.desky.shared"
    compileSdk = 36

    defaultConfig {
        minSdk = 26
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
