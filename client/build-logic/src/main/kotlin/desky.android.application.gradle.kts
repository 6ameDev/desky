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

    // Lint gating: errors fail the build; triaged violations are absorbed
    // by the checked-in baseline, so only NEW issues break the gate.
    lint {
        abortOnError = true
        warningsAsErrors = false
        checkReleaseBuilds = false
        baseline = file("lint-baseline.xml")
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        // Advisory: warnings logged, build stays green.
        // Flip to true in a later enforcement pass once clean.
        allWarningsAsErrors.set(false)
    }
}
