import org.jetbrains.kotlin.gradle.dsl.KotlinMultiplatformExtension

plugins {
    id("desky.kmp.base")
    alias(libs.plugins.ksp)
    alias(libs.plugins.room3)
    alias(libs.plugins.skie)
    alias(libs.plugins.mokkery)
    alias(libs.plugins.kotlin.cocoapods)
    // Phase 1 static analysis (issue 54, task 5 — advisory only, promotion in
    // task 9). Versions come from the root catalog.
    alias(libs.plugins.detekt)
    alias(libs.plugins.ktlint)
    // Kover: report tasks only, no verification bounds (baseline first, task 9
    // decides enforcement). Applied at root for aggregation; applied here
    // (version from catalog) so JVM test tasks are instrumented.
    alias(libs.plugins.kover)
}

// T1 skeleton: dependency/config wiring only. No entities, DAOs, or code yet.
// Room3/KSP processor wiring (room3-compiler) lands in T5 with the data layer.
// The Room3 Gradle plugin mandates a schema directory even with no @Database yet.
room3 {
    schemaDirectory("$projectDir/schemas")
}

kotlin {
    // T8 iOS bootstrap: IosKoin.kt (initKoinForIos + typed VM accessors) lives
    // under client/iosApp/Kotlin/ and compiles into iosMain here, so it ships
    // inside the shared framework/pod. client/shared/src stays frozen.
    // NOTE: `all` + name guard (not `named`) — the iosMain intermediate
    // source set is not yet present when this body first executes under the
    // cocoapods plugin; `all` also covers objects added later.
    sourceSets.all {
        if (name == "iosMain") {
            kotlin.srcDir("../iosApp/Kotlin")
        }
    }

    cocoapods {
        summary = "Desky shared KMP module (domain + data + presentation view models)"
        homepage = "https://github.com/6ameDev/desky"
        version = "0.1.0"
        ios.deploymentTarget = "16.0"
        podfile = project.file("../iosApp/Podfile")
        framework {
            baseName = "shared"
        }
    }

    sourceSets {
        commonMain.dependencies {
            implementation(libs.kotlinx.coroutines.core)
            implementation(libs.koin.core)
            implementation(libs.koin.core.viewmodel)
            implementation(libs.lifecycle.viewmodel)
            implementation(libs.ktor.client.core)
            implementation(libs.room3.runtime)
            implementation(libs.sqlite.bundled)
            implementation(libs.cryptography.core)
            implementation(libs.cryptography.provider.optimal)
        }
        commonTest.dependencies {
            implementation(libs.kotlin.test)
            implementation(libs.turbine)
            implementation(libs.mokkery.core)
            implementation(libs.kotlinx.coroutines.test)
        }
        // NOTE: ktor engines (okhttp/darwin) stay catalog-only for now.
        // ktor-client-okhttp 3.6.0 pulls okhttp-android 5.5.0, which mandates
        // compileSdk 37; only platform android-36 is installed and the brief
        // pins compileSdk 36 with no toolchain installs. T3/T5 wire engines.
        androidMain.dependencies {
            // Bond PSK vault backend (EncryptedSharedPreferences + MasterKey).
            implementation(libs.security.crypto)
        }
    }
}

// T5 data layer: Room3 KSP processor wiring, per the official Room KMP setup
// (one ksp config per target; NO kspCommonMainMetadata — that config makes
// Room emit the constructor `actual` into common metadata, which then has no
// matching `expect`). Each per-target run generates the platform `actual`
// for the expect DeskyDatabaseConstructor below plus the platform `*_Impl`.
dependencies {
    add("kspAndroid", libs.room3.compiler)
    add("kspJvm", libs.room3.compiler)
    add("kspIosArm64", libs.room3.compiler)
    add("kspIosSimulatorArm64", libs.room3.compiler)
}

// detekt 1.23.8 does not auto-detect KMP source sets under Kotlin 2.4 (task
// runs NO-SOURCE without this), so wire every Kotlin source set explicitly —
// commonMain/androidMain/jvmMain/iosMain plus tests — minus KSP-generated
// output under build/ (machine output, never hand-triaged).
val kmpKotlinSources = files(provider {
    val buildPath = project.layout.buildDirectory.get().asFile.invariantSeparatorsPath
    extensions.getByType(KotlinMultiplatformExtension::class.java)
        .sourceSets.flatMap { it.kotlin.sourceDirectories }
        .filter { !it.invariantSeparatorsPath.startsWith(buildPath) }
})

// Phase 1 static analysis (issue 54, task 5): advisory only, promotion in
// task 9. detekt owns analysis (formatting/ktlint sets are absent from
// config/detekt/detekt.yml — the ktlint plugin owns format).
detekt {
    toolVersion = libs.versions.detekt.get()
    config.setFrom(files("${rootProject.projectDir}/config/detekt/detekt.yml"))
    baseline = rootProject.file("config/detekt/baseline.xml")
    ignoreFailures = true
    source = kmpKotlinSources
}

ktlint {
    ignoreFailures.set(true)
}

// Kover: report tasks only, no verification bounds (baseline first, task 9
// decides enforcement). Applied at root; subprojects inherit instrumentation.
