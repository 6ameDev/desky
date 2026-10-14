plugins {
    `kotlin-dsl`
}

repositories {
    gradlePluginPortal()
    google()
    mavenCentral()
}

// Only the plugins applied by the convention scripts below live on this
// compile classpath. KSP / Room3 / SKIE are applied directly in :shared via
// the root catalog — their jars carry newer Kotlin metadata than the Kotlin
// embedded in Gradle 8.14, which breaks build-logic script compilation.
dependencies {
    implementation(libs.kotlin.gradle.plugin)
    implementation(libs.android.gradle.plugin)
}
