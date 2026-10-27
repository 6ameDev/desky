# iosApp (T8)

Xcode SwiftUI shell for the desky client, over the shared KMP module.

## Layout

- `iosApp/` — Swift sources (4 files): `DeskyApp.swift` (@main entry, boots
  Koin once via `DiIosKoinKt.doInitKoinForIos()` and owns the three shared
  ViewModel instances), `PairingView.swift`, `ControlView.swift` (drag
  joystick normalized -1…1 + recenter, telemetry bar, E-STOP + release),
  `SettingsView.swift`. `Info.plist` (bundle keys incl.
  `NSLocalNetworkUsageDescription` — iOS shows the Local Network prompt when
  the app talks UDP to the robot on the LAN; without the key the prompt has
  no text and traffic is blocked on device).
- `Kotlin/IosKoin.kt` — the single Kotlin entry point (`initKoinForIos()` +
  typed `Koin.pairing/robotControl/settingsViewModel()` accessors). It lives
  here (not `shared/src`, which is frozen) and compiles into `:shared`'s
  `iosMain` via a `srcDir` in `shared/build.gradle.kts`, shipping inside the
  `shared` framework/pod.
- `iosApp.xcodeproj` (+ shared scheme `iosApp`), `Podfile`
  (`pod 'shared', :path => '../shared'`, iOS 16.0).

## Regenerate from scratch

```sh
./gradlew :shared:podspec && ./gradlew :shared:generateDummyFramework
cd iosApp && pod install
xcodebuild -workspace iosApp.xcworkspace -scheme iosApp \
  -configuration Debug -destination 'generic/platform=iOS Simulator' build
```

`Pods/`, `*.xcworkspace` and `shared/shared.podspec` are derived (gitignored);
`Podfile` + `Podfile.lock` + the xcodeproj + Swift sources are tracked.

## Lint (advisory)

Pinned via Mint (`client/Mintfile`): `mint bootstrap`, then
`mint run swiftlint lint iosApp` / `mint run swiftformat --lint iosApp`.
Periphery runs separately with an indexed build (see `client-ios.yml`).

## Notes

- Swift consumes the framework via SKIE 0.10.15: `StateFlow` as
  `AsyncSequence` (`for await` in `.task`), suspend funcs as `async/throws`
  (none called yet — all VM intents are plain sync funcs), Kotlin enums as
  Swift enums. Each view owns an `ObservableObject`/`@StateObject` wrapper
  holding the shared VM instance.
- `viewModel { }` is factory-scoped in Koin 4, so `IosKoin.kt` resolves each
  VM once and shares the instances (one `RobotControlViewModel` heartbeat
  loop app-wide). Verified on JVM during T8.
- Portrait-only on iPhone for v1 (`UISupportedInterfaceOrientations`).
- Running on a simulator needs a downloaded simulator runtime
  (`xcrun simctl list runtimes`); building needs only the SDK.
