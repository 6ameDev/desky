# desky-client architecture & platform guide

**Mental model in one paragraph:** `:shared` is the whole app minus pixels — protocol codecs, repositories, feature logic, ViewModels, and DI. `androidApp` and `iosApp` are thin native shells that render `UiState` and forward intents; they own zero business logic. The robot is a dumb UDP endpoint (6-byte control at ~50 Hz, 9-byte telemetry out at ~15 Hz, port 3333) with a ~500 ms stale-command failsafe — the client must heartbeat continuously.

## 1. Stack & Boundaries

| Concern | Choice | Why it exists |
|---|---|---|
| Language/Toolchain | Kotlin 2.x (K2), Gradle wrapper, JDK 17 | Single language for cross-platform logic |
| Architecture | Feature-First Clean Architecture | Groups code by product capability (`features/pairing`, `features/control`) to prevent flat file bloat |
| State & UI Flow | Coroutines + `StateFlow`, UDF | One immutable `StateFlow<UiState>` per screen + explicit intents |
| DI | Koin (`core`, `viewmodel`, `android`, `compose`) | Shared dependency graph across targets |
| Persistence | Room3 (`androidx.room3`) + `sqlite-bundled` | Paired device storage & local settings |
| Networking | Raw UDP sockets (no HTTP); Ktor client-core | Sub-50 ms control loop |
| Android UI | Compose BOM + Navigation | Single-activity, lifecycle-aware collection |
| iOS Bridge | `native.cocoapods` export + SKIE | SKIE turns `StateFlow` into Swift `AsyncSequence` and suspend into async/await |
| iOS UI | SwiftUI, `ObservableObject` wrappers | Passive rendering wrappers (iOS 16+ floor) |

### Critical SKIE House Rule
> **Never put `suspend` functions inside a Kotlin `object` singleton.** SKIE fails to generate correct Swift static members for object instances, breaking `syncFramework`. Always use top-level `suspend` functions or regular class instances.

---

## 2. Feature-First Directory Layout (`commonMain`)

```text
client/shared/src/commonMain/kotlin/com/desky/shared/
├── core/                  // Cross-cutting networking and utilities
├── data/                  // Database schemas, local storage, and repository implementations
├── domain/                // Pure business models and repository interfaces
└── features/              // Feature-centric slices (UI logic, ViewModels, feature state)
    ├── pairing/
    ├── control/
    └── settings/
```

## 3. Runtime Composition (Koin Graphs)

The Koin dependency graphs are identical across platforms, but **ownership differs deliberately**:

- **Android:** Koin `viewModel {}` is factory-scoped per destination.
    
- **iOS:** `initKoinForIos()` resolves and caches ViewModels for the app lifetime inside `SharedGraph` to prevent state-forking and duplicate UDP heartbeat loops.
    

Both platforms share the singleton `ioScope` (`SupervisorJob` + `Dispatchers.Default`) feeding `UdpSocketManager`.

## 4. Key Data Flows

- **Control Loop:** Joystick input → `RobotControlUiState` → 20 ms tick loop → `UdpSocketManager.send()` → Robot `:3333`. (Never pauses; maintains the 500 ms failsafe).
    
- **Telemetry:** Inbound datagrams → `ObserveTelemetryUseCase` → `StateFlow` updates → Compose / SwiftUI status bars.
    
- **Pairing Auth:** 6-digit code → mDNS broadcast (`PAIR_REQ`) → AES-GCM decryption of PSK (`PAIR_OK`) → Secure platform vault (Keystore/Keychain) + Room device metadata.
    

## 5. Essential Build Commands

|**Goal**|**Command (from client/)**|**Proves**|
|---|---|---|
|Unit tests|`./gradlew :shared:jvmTest`|Runs `commonTest` suite on JVM|
|Assemble all targets|`./gradlew :shared:assemble`|JVM, Android AAR, and iOS frameworks|
|Android debug APK|`./gradlew :androidApp:assembleDebug`|Compose wiring and compile health|
|Vuln scan (advisory)|`make vuln`|OSV-Scanner SARIF over resolved Gradle deps (see below)|
|iOS framework sync|`./gradlew :shared:syncFramework -Pkotlin.native.cocoapods.platform=iphonesimulator -Pkotlin.native.cocoapods.archs=arm64 -Pkotlin.native.cocoapods.configuration=Debug`|Generates framework for Xcode|

_Local Swift iteration tip:_ Set `COCOAPODS_SKIP_KOTLIN_BUILD=YES` in Xcode to skip Kotlin rebuilds during pure-Swift UI work. Remember to unset and rebuild after touching `shared/`.

Dependency vulnerabilities (`make vuln`, issue 54 task 8 — advisory, never
gates `check`): scans the resolved Gradle deps of `:shared` + `:androidApp`
via ephemeral `--write-locks` lockfiles (activated scan-only by `-PosvLocks`
in the root build file, deleted afterwards, nothing committed). Baseline
27 Oct 2026: 110 findings / 55 distinct CVEs, 0 fixed, 0 suppressed —
upgrades are out of scope until the task-9 gate, so no dependency version
may change in a scan-only change. NOT scanned: the iOS `shared` pod is
path-local (nothing for OSV to resolve — use `pod outdated` in `iosApp/`),
and embedded PlatformIO `lib_deps` git URLs sit outside OSV Gradle-lockfile
scope. Alternatives: autonomousapps dependency-analysis is deferred (AGP 9.3
+ Kotlin 2.4 run ahead of its tested matrix), OWASP Dependency-Check is
rejected (slow, needs an NVD key, noisy CPE matching), Snyk is rejected
(needs a token). Task 9 keeps this advisory: warn-only threshold hack stays,
plus SARIF upload + scheduled scan (see the checklist below).

Static analysis gates (issue 54, task 9 — gating vs advisory):

| Tool | State | How it gates |
|---|---|---|
| Android Lint | GATING (`abortOnError = true`) | `lint-baseline.xml` absorbs the 3 triaged IDs (1 error + 1 warning + 1 hint); only NEW issues fail `:androidApp:lintDebug`, which runs in `make check` and CI |
| detekt | GATING (`ignoreFailures = false`) | `config/detekt/baseline.xml` absorbs the 531 triaged IDs (verified zero-issue run before the flip); only NEW issues fail `detekt` in CI |
| ktlint | ADVISORY (`ignoreFailures = true`) | ~4080 violations logged, format never run — flipping now would red the build, so it stays green until triaged |
| Kover | REPORT-ONLY | no verification bounds; `:shared:koverXmlReport` in CI uploads the report |
| OSV-Scanner | ADVISORY | warn-only count/threshold switch; SARIF uploaded in CI, no threshold |
| iOS analyze / concurrency / SwiftLint / SwiftFormat / Periphery | ADVISORY | all `continue-on-error` report-only in CI with log artifacts; no baselines triaged yet |

Promotion checklist (each line flips independently when its condition holds):
- ktlint: triage the backlog → run `./gradlew ktlintFormat` → verify `ktlintCheck` clean → flip `ignoreFailures` to false in `shared/` + `androidApp/` build files.
- Kover: set verification bounds only after N releases of stable coverage data (no invented threshold).
- OSV: replace the warn-only hack with a JSON threshold + real bound, add a scheduled scan; do not invent a threshold before triage.
- iOS: trip each tool to gating only after a local baseline is triaged (same absorb-then-gate pattern as lint/detekt).
- Embedded installs (clang-tidy/cppcheck): CI installs best-effort with `continue-on-error`; the `make check` firmware gate never depends on them.

Baseline regeneration (generated XML baselines are rewritten whole by their
tools, so in-file comments would not survive — these commands are the record):
- `client/androidApp/lint-baseline.xml`: `./gradlew :androidApp:lintDebug`, then update the baseline from the report (Android Studio: suppress-as-baseline) — never hand-edit IDs.
- `client/config/detekt/baseline.xml`: `./gradlew detektBaseline` (merges current findings into the baseline) — review the diff, triage, commit.
- `embedded/tools/cppcheck-suppressions.txt`: hand-maintained, header comment inside the file is the record — append `<error-id>:<file-glob>` with a justification comment per line.
- `embedded/.clang-tidy`: hand-maintained single config (upward discovery covers core/head/shared) — header comment inside the file is the record.

Pre-commit is opt-in only (`pre-commit install` from the repo root; root
`.pre-commit-config.yaml` mirrors the CI commands with mono-repo `files:`
filters). CI does NOT run pre-commit itself — workflows invoke the
underlying commands directly.

## 6. Where to look next

- Wire truth: `shared/protocol/README.md` → `embedded/.../udp_codec.h` → `domain/Codec.kt` (+`Models.kt`).
- Byte order: `data/Ipv4.kt` KDoc first. Socket contract: `domain/UdpSocketManager.kt` + platform actuals.
- State machines: `presentation/RobotControlViewModel.kt` (heartbeat/estop/unpair), `PairingViewModel.kt`, `SettingsViewModel.kt`.
- Wiring: `di/AppModules.kt`, Android `DeskyApp.kt` + `ui/DeskyNavHost.kt`, iOS `Kotlin/IosKoin.kt` + `DeskyApp.swift`.
- UI pattern: stateful `XScreen` + stateless `XContent` + `@Preview` (Android); `ObservableObject` wrapper + `.task { for await }` (iOS).
