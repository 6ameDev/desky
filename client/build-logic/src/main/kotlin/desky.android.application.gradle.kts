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

    // Phase 0 static analysis (issue 54): warn-only. Lint errors do not fail
    // the build yet; violations are triaged via the checked-in baseline.
    // Promotion to errors is a later planned phase with a clean baseline.
    lint {
        abortOnError = false
        warningsAsErrors = false
        checkReleaseBuilds = false
        baseline = file("lint-baseline.xml")
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
        // Phase 0 advisory: warnings logged, build stays green.
        // Flip to true in the later enforcement phase once clean.
        allWarningsAsErrors.set(false)
    }
}
