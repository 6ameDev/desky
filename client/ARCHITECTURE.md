# desky-client architecture & platform guide

**Mental model in one paragraph:** `:shared` is the whole app minus pixels — protocol codecs, repositories, feature logic, ViewModels, and DI. `androidApp` and `iosApp` are thin native shells that render `UiState` and forward intents; they own zero business logic. The robot is a dumb UDP endpoint (6-byte control at ~50 Hz, 9-byte telemetry out at ~15 Hz, port 3333) with a ~500 ms stale-command failsafe — the client must heartbeat continuously.

## 1. Stack & Boundaries

| Concern | Choice | Why it exists |
|---|---|---|
| Language/Toolchain | Kotlin 2.x (K2), Gradle wrapper, JDK 17 | Single language for cross-platform logic |
| Architecture | Slices over shared kernel | `core/` kernel plus `features/<name>` slices (flat today, internal layers only on graduation triggers) |
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
├── core/                  // Shared kernel: protocol, crypto, counters, transport/vault boundaries,
│                          // repository interfaces, Room impls, IPv4, socket factory
├── data/                  // REMOVED (Phase 0): merged into core/
├── domain/                // REMOVED (Phase 0): merged into core/
├── di/                    // kernel module + one Koin module per feature
└── features/              // Feature slices, flat until graduation triggers fire
    ├── pairing/
    ├── control/
    ├── settings/
    └── session/           // cross-cutting unpaired fence
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
|Vuln scan (advisory)|`make vuln`|Advisory dependency vuln report|
|iOS framework sync|`./gradlew :shared:syncFramework -Pkotlin.native.cocoapods.platform=iphonesimulator -Pkotlin.native.cocoapods.archs=arm64 -Pkotlin.native.cocoapods.configuration=Debug`|Generates framework for Xcode|

_Local Swift iteration tip:_ Set `COCOAPODS_SKIP_KOTLIN_BUILD=YES` in Xcode to skip Kotlin rebuilds during pure-Swift UI work. Remember to unset and rebuild after touching `shared/`.

`make vuln` scans resolved Gradle deps of `:shared` + `:androidApp` via ephemeral lockfiles (created scan-only, deleted afterwards, never committed); advisory, never gates `check`. The iOS `shared` pod is path-local — use `pod outdated` in `iosApp/` instead. Live results live in the CI SARIF artifact.

Static analysis gates (gating vs advisory):

| Tool | State | How it gates |
|---|---|---|
| Android Lint | GATING | `lint-baseline.xml` absorbs triaged IDs; only new issues fail `:androidApp:lintDebug` in `make check` and CI |
| detekt | GATING | `config/detekt/baseline.xml` absorbs triaged IDs; only new issues fail `detekt` in CI |
| ktlint | ADVISORY | violations logged, stays green until triaged and formatted |
| Kover | REPORT-ONLY | no verification bounds; `:shared:koverXmlReport` uploads the report |
| OSV-Scanner | ADVISORY | warn-only; SARIF uploaded in CI, no threshold |
| iOS analyze / concurrency / SwiftLint / SwiftFormat / Periphery | ADVISORY | `continue-on-error` report-only in CI with log artifacts |

Promotion checklist (each line flips independently when its condition holds):
- ktlint: triage the backlog → run `./gradlew ktlintFormat` → verify `ktlintCheck` clean → flip `ignoreFailures` to false in `shared/` + `androidApp/` build files.
- Kover: set verification bounds only after N releases of stable coverage data.
- OSV: replace the warn-only switch with a JSON threshold + real bound, add a scheduled scan.
- iOS: trip each tool to gating only after a local baseline is triaged (same absorb-then-gate pattern as lint/detekt).

Baseline regeneration (generated baselines are rewritten whole by their tools — these commands are the record):
- `client/androidApp/lint-baseline.xml`: `./gradlew :androidApp:updateLintBaseline` — never hand-edit IDs.
- `client/config/detekt/baseline.xml`: `./gradlew detektBaseline` — review the diff, triage, commit.
- Embedded baselines: see `embedded/docs/static-analysis.md`.

Baseline no-growth guard (`tools/baseline-guard.sh`, `make baseline-guard`, gating in CI): caps on finding counts (lint `<issue` entries, detekt `<ID>` entries) fail the run when either baseline grows. Fix new findings first; a genuinely legacy finding earns a cap bump only as its own reviewed change alongside the regenerated baseline — never silent growth.

Pre-commit is opt-in only (`pre-commit install` from the repo root; root
`.pre-commit-config.yaml` mirrors the CI commands with mono-repo `files:`
filters). CI does NOT run pre-commit itself — workflows invoke the
underlying commands directly.

## 6. Where to look next

- Wire truth: `shared/protocol/` (repo root) → `embedded/.../udp_codec.h` → `core/Codec.kt` (+`Models.kt`).
- Byte order: `data/Ipv4.kt` KDoc first. Socket contract: `domain/UdpSocketManager.kt` + platform actuals.
- State machines: `presentation/RobotControlViewModel.kt` (heartbeat/estop/unpair), `features/pairing/PairingViewModel.kt`, `features/settings/SettingsViewModel.kt`, `features/session/SessionGateViewModel.kt`.
- Wiring: `di/AppModules.kt`, Android `DeskyApp.kt` + `ui/DeskyNavHost.kt`, iOS `Kotlin/IosKoin.kt` + `DeskyApp.swift`.
- UI pattern: stateful `XScreen` + stateless `XContent` + `@Preview` (Android); `ObservableObject` wrapper + `.task { for await }` (iOS).
