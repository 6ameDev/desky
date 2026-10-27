plugins {
    id("desky.android.application")
    alias(libs.plugins.kotlin.compose)
    // Phase 1 static analysis (issue 54, task 5 — detekt promoted to gating
    // in task 9; ktlint stays advisory). Versions come from the root catalog.
    alias(libs.plugins.detekt)
    alias(libs.plugins.ktlint)
    // Kover: see shared/build.gradle.kts — report only, no bounds.
    alias(libs.plugins.kover)
}

android {
    buildFeatures {
        compose = true
    }
}

// Phase 0 static analysis (issue 54): Compose stability reports as artifact,
// never gating. Promote to warn/error only after a baseline is triaged.
composeCompiler {
    reportsDestination.set(layout.buildDirectory.dir("compose-reports"))
    metricsDestination.set(layout.buildDirectory.dir("compose-reports"))
}

dependencies {
    implementation(project(":shared"))
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.material3)
    implementation(libs.compose.material.icons.core)
    implementation(libs.compose.ui.tooling.preview)
    implementation(libs.activity.compose)
    implementation(libs.lifecycle.runtime.compose)
    implementation(libs.navigation.compose)
    implementation(libs.camera.core)
    implementation(libs.camera.camera2)
    implementation(libs.camera.lifecycle)
    implementation(libs.camera.view)
    implementation(libs.mlkit.barcode.scanning)
    implementation(libs.koin.android)
    implementation(libs.koin.androidx.compose)
}

// Phase 1 gating (issue 54, task 9): analysis owns failures — the checked-in
// baseline absorbs the 531 triaged IDs, so only NEW issues break the gate.
// (formatting/ktlint sets stay OFF in config/detekt/detekt.yml — the ktlint
// plugin owns format).
detekt {
    toolVersion = libs.versions.detekt.get()
    config.setFrom(files("${rootProject.projectDir}/config/detekt/detekt.yml"))
    baseline = rootProject.file("config/detekt/baseline.xml")
    ignoreFailures = false
}

ktlint {
    ignoreFailures.set(true)
}
