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
|iOS framework sync|`./gradlew :shared:syncFramework -Pkotlin.native.cocoapods.platform=iphonesimulator -Pkotlin.native.cocoapods.archs=arm64 -Pkotlin.native.cocoapods.configuration=Debug`|Generates framework for Xcode|

_Local Swift iteration tip:_ Set `COCOAPODS_SKIP_KOTLIN_BUILD=YES` in Xcode to skip Kotlin rebuilds during pure-Swift UI work. Remember to unset and rebuild after touching `shared/`.
