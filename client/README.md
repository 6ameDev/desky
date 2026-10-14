# desky-client — KMP companion app for the desky robot

100% shared business logic in `:shared`, 100% native UI (Jetpack Compose
on Android, SwiftUI on iOS). Talks to the robot over the frozen binary UDP
protocol — see `../shared/protocol/README.md` for the wire format.

New here? Start with [`ARCHITECTURE.md`](ARCHITECTURE.md): stack by
concern, module/target map, runtime Koin graphs, data flows, build-task map.

## Prerequisites (tools a human installs)

- JDK 17 — pinned via `client/.mise.toml` (`temurin-17`); no system Gradle
  (wrapper-only).
- Android SDK with platform 36 — resolved via `ANDROID_HOME`
  (no `local.properties`).
- Xcode 26 + CocoaPods 1.17 — iOS shell only.

Dependency versions live in exactly one place: `gradle/libs.versions.toml`.
Two constraints that bite if you touch them: KSP requires AGP ≥ 8.12, and
the Compose BOM must stay on the 2026.04 line until compileSdk moves past 36.

## Layout

```
client/
├── gradle/libs.versions.toml   # single source of truth for all pins
├── build-logic/                # convention plugins (KMP base, Android app)
├── shared/                     # all logic: domain → data → presentation → di
├── androidApp/                 # Compose UI (single activity + navigation)
└── iosApp/                     # SwiftUI + xcodeproj, consumes `shared` via Pods
```

Full map in [`ARCHITECTURE.md`](ARCHITECTURE.md). Layering rule: domain
depends on nothing, data implements domain interfaces, presentation exposes
one immutable `StateFlow<UiState>` per screen, UI shells only render and send
intents.

## Build & test (from `./client/`)

Stale shells miss the mise shims — prefix every Gradle/CocoaPods/Xcode
command with:

```bash
export PATH="$HOME/.local/share/mise/shims:/opt/homebrew/bin:$PATH"
export ANDROID_HOME="$HOME/Library/Android/sdk" ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
```

```bash
./gradlew :shared:jvmTest                  # unit tests (commonTest on JVM)
./gradlew :shared:assemble                  # every target (JVM/Android/iOS frameworks)
./gradlew :androidApp:assembleDebug         # APK
./gradlew :shared:syncFramework -Pkotlin.native.cocoapods.platform=iphonesimulator \
  -Pkotlin.native.cocoapods.archs=arm64 -Pkotlin.native.cocoapods.configuration=Debug
cd iosApp && pod install                    # regenerates Pods/ + .xcworkspace (gitignored)
xcodebuild -workspace iosApp.xcworkspace -scheme iosApp -configuration Debug \
  -destination 'platform=iOS Simulator,name=iPhone 16' build   # needs a simulator runtime
```

## Protocol caveats (can brick a demo)

- The drive loop must never pause: the robot failsafes ~500 ms after the
  last datagram, so the app heartbeats centered frames even with untouched
  sticks.
- Discovery must send a *valid* 6-byte control frame — the firmware drops
  malformed datagrams, so empty/arbitrary hellos never pin the unicast peer.
- Pairing PINs are **not real security**: the token is derived client-locally
  and the firmware speaks open UDP (`TODO(firmware-auth)` in code).

## Deferred-run checklist (needs hardware or downloads)

- [ ] Simulator runtime → xcodebuild build above, then boot + smoke all tabs.
- [ ] Android emulator → install the debug APK, smoke: pair → drive → settings → forget.
- [ ] Physical robot on AP `desky`: pair `192.168.4.1`, verify heartbeat +
      telemetry + E-STOP + failsafe on app kill.
