// Plugin version declarations only. Versions live in gradle/libs.versions.toml.
plugins {
    alias(libs.plugins.kotlin.multiplatform) apply false
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.ksp) apply false
    alias(libs.plugins.room3) apply false
    alias(libs.plugins.skie) apply false
    alias(libs.plugins.kotlin.cocoapods) apply false
    // Phase 1 static analysis (issue 54, task 5 — all advisory, promotion in
    // task 9): detekt + ktlint are declared here but applied per-module (see
    // shared/androidApp build files); kover is applied at root for aggregation.
    alias(libs.plugins.detekt) apply false
    alias(libs.plugins.ktlint) apply false
    alias(libs.plugins.kover)
}
