// Plugin version declarations only. Versions live in gradle/libs.versions.toml.
plugins {
    alias(libs.plugins.kotlin.multiplatform) apply false
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.ksp) apply false
    alias(libs.plugins.room3) apply false
    alias(libs.plugins.skie) apply false
    alias(libs.plugins.kotlin.cocoapods) apply false
    // Phase 1 static analysis (issue 54, task 5 — detekt promoted to gating
    // in task 9; ktlint stays advisory, kover report-only).
    alias(libs.plugins.detekt) apply false
    alias(libs.plugins.ktlint) apply false
    alias(libs.plugins.kover)
    // Vulnerability scanning (issue 54, task 8 — advisory only, promotion in
    // task 9): applied at root so `osvScan` covers all modules.
    alias(libs.plugins.osv.scanner)
}

// OSV-Scanner advisory config: SARIF report into build/osv-scanner/ (locked
// decision: SARIF-native, no NVD key, no token).
osvScanner {
    format.set("sarif")
    // Warn-only switch: `count` mode looks for JSON `results` (absent from
    // SARIF output), so the count is always 0 and this threshold never
    // trips — findings print into the report but never fail the build.
    // Binary errors (exit >= 127) still fail loudly. Task 9 replaces this
    // with JSON + a real threshold + SARIF upload + scheduled scan.
    failOn.set("count")
    failOnThreshold.set(1000000.0)
}

// Scan-only dependency locking: -PosvLocks activates lockAllConfigurations
// so `make vuln` can `--write-locks` for osv-scanner. Absent from normal
// builds (check/test/lint), so resolution behavior there is untouched and no
// lockfiles are ever committed — the scan deletes them afterwards.
if (providers.gradleProperty("osvLocks").isPresent()) {
    subprojects {
        dependencyLocking {
            lockAllConfigurations()
        }
    }
}
