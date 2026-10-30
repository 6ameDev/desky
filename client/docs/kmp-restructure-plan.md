# KMP Restructure Plan: Slices Over Shared Kernel (Hybrid-Ready)

Status: locked. Scope: `client/shared` commonMain plus mirrors in tests, DI, and thin shells. No protocol, firmware, or behavior changes.

## 1. Context

The client is at POC stage. Roughly 91 tracked Kotlin plus Swift files, 3 product features plus a session fence, one Room database (version 6, 2 tables), one UDP transport, one crypto suite, 2 era-aware counters.

Current `commonMain` is layer-first: `domain/` holds 25 flat files, `presentation/` holds 4 flat ViewModels, `data/` holds Room repos plus the `expect` socket factory. There is no `features/` and no `core/` on disk.

Docs already promise feature-first (`client/AGENTS.md` § Architecture & Packaging Rules, `client/ARCHITECTURE.md` §2). The code drifted from the docs. This plan closes that gap without over-engineering for scale we do not have yet.

Prior proposals evaluated:

- Pure layer-first (status quo): does not scale past POC, 25-file flat domain folder hides feature ownership.
- Pure feature-first with collapsed services: would fragment protocol-pinned code and delete tested orchestration.
- Full hybrid with per-feature presentation/domain/data from day one: 9-plus directories for ~30 shared files, splits single-DB Room ownership and cross-feature repositories for no isolation gain.

Decision: feature slices over a shared kernel. Flat feature folders today, reserved internal layer slots for later. This sets the path to full hybrid while keeping POC velocity.

## 2. Goals and Non-Goals

Goals:

- Feature folders own screen state plus private orchestration.
- Shared protocol, transport, persistence, and sequencing stay in one kernel.
- Every future feature starts in the same shape and graduates to internal layers only when complexity earns it.
- Moves are additive: adding layers inside one feature never forces moves in others.

Non-goals:

- No behavior changes to pairing auth, control loop, telemetry, ALIVE cadence, MANUAL gating, e-stop, unpair, refresh, or disconnect handling.
- No Room schema, migration, DAO, or entity changes.
- No `expect`/`actual` signature changes.
- No SKIE-visible suspend signature changes beyond package moves.
- No new telemetry backend, design system, or generic util bucket.

## 3. Current Inventory (What Must Be Covered)

### 3.1 Domain (25 files, `com.desky.shared.domain`)

Kernel (cross-feature, stays in kernel):

- `Models.kt` (140 lines): RobotMode, ControlCommand, Telemetry, PairedDevice, RobotConfig, ExpressionEntry.
- `Codec.kt` (736 lines): wire constants plus Control/Pairing/Telemetry codecs. Pinned to `embedded/.../udp_codec.h`.
- `Outcome.kt` (11 lines): SKIE-visible Result replacement.
- `UdpDatagram.kt` (25 lines): raw socket payload type.
- `UdpSocketManager.kt` (21 lines): untyped transport boundary.
- `UdpSocketBinder.kt` (55 lines): platform Wi-Fi pinning hook.
- `PairSecretStore.kt` (41 lines): platform vault boundary, sole at-rest home of PSK.
- `DeviceRepository.kt` (39 lines): single paired-device persistence.
- `SettingsRepository.kt` (12 lines): robot tuning persistence.
- `Sha256.kt` (117 lines): legacy token path only.
- `PairingCrypto.kt` (489 lines): all pairing/auth v1 crypto as top-level suspend funs. Must stay top-level per SKIE rule.
- `QrCodeParser.kt` (32 lines): bare 6-digit only by design.
- `ToRobotCounter.kt` (246 lines): mutex-guarded send-sequence source.
- `FromRobotCounter.kt` (102 lines): mutex-guarded receive-sequence verifier.

Use cases (triage individually, see §6):

- `PairDeviceWithCodeUseCase.kt` (205 lines): retry plus HKDF plus AES-GCM. Keep.
- `PairRefreshUseCase.kt` (146 lines): silent era plus PSK rotation. Keep.
- `ForgetDeviceUseCase.kt` (103 lines): 3x UNPAIR plus Room plus vault plus counter reset. Keep.
- `ObserveRemoteDisconnectUseCase.kt` (148 lines): HMAC-verified disconnect. Keep.
- `AliveUseCase.kt` (62 lines): keepalive plus goodbye. Keep.
- `SendSignedControlUseCase.kt` (32 lines) plus `EmergencyStopUseCase.kt` (36 lines): merge into one control sender.
- `ManualModeUseCases.kt` (84 lines): Take plus Release, already grouped. Keep grouped.
- `ExpressionUseCases.kt` (92 lines): catalog constants plus trigger. Keep grouped.
- `GetPairPskUseCase.kt` (25 lines): single vault read. Fold only after confirming single caller.
- `DeviceConfigUseCases.kt` (30 lines): save/observe passthrough. Fold only after confirming single caller.
- `ObserveTelemetryUseCase.kt` (18 lines): socket-to-telemetry mapping. Keep (shared by control loop and tests).

### 3.2 Presentation (4 files, `com.desky.shared.presentation`)

- `RobotControlViewModel.kt` (1193 lines): drive loop at 50 Hz down to 5 Hz idle, telemetry collector, ALIVE cadence, MANUAL gating, e-stop, unpair, refresh trigger, disconnect handling. 8 constructor deps plus 6 `getOrNull` optionals. The actual bloat. Decompose inside the feature, do not just move.
- `PairingViewModel.kt` (120 lines): PIN plus pair/unpair plus QR scan intent. Move as-is.
- `SettingsViewModel.kt` (98 lines): sliders/fields plus mutex-serialized persist. Move as-is.
- `SessionGateViewModel.kt` (53 lines): global unpaired fence, armed on first emission. Cross-cutting, gets its own `session/` home.

UDF conformance is good (single `StateFlow<UiState>` plus intents per screen). Preserve it.

### 3.3 Data and Platform Actuals

Common:

- `data/db/DeskyDatabase.kt` (version 6, destructive-migration policy), `Daos.kt`, `Entities.kt` with legacy `192.168.4.1` to `desky.local` mapping.
- `RoomSettingsRepository.kt`, `RoomDeviceRepository.kt`.
- `UdpSocketFactory.kt` (`expect`), `Ipv4.kt`.

Platform (do not touch signatures):

- androidMain: `DatagramSocketManager`, `WifiSocketBinder` (Wi-Fi-only request, 1 s debounced `networkAvailable`), `EncryptedPairSecretStore`, Room builder.
- iosMain: `PosixUdpSocketManager` (per-send `getaddrinfo` resolve), `KeychainPairSecretStore` (manual CFRelease), Room builder.
- jvmMain: `JvmUdpSocketManager`, `InMemoryPairSecretStore` (test/desktop only).

`WifiRejoinMonitor.swift` (NWPathMonitor, monitor-only, 1 s debounce) stays Swift. Its knock target `onForeground()` / `networkAvailable` already lives in common. Do not port the monitor.

### 3.4 DI, Tests, Shells

- `di/AppModules.kt`: `dataModule` plus `domainModule` (2 counter singletons plus 12 use-case factories) plus `presentationModule` (4 Koin 4 `viewModel{}` entries, `RobotControlViewModel` with `getOrNull` optionals so JVM/iOS graphs without binder still resolve).
- `iosApp/Kotlin/IosKoin.kt`: compiled into iosMain via `srcDir` hack, `initKoinForIos` plus typed accessors, NSLock-guarded. Swift `SharedGraph` caches ViewModels app-lifetime (Android uses per-destination factory).
- Tests: 19 domain tests, 7 presentation files including `TestDoubles.kt`, 2 data tests, 1 JVM secret-store test. Packages mirror source and must move in lockstep.
- Shells: Android Compose (`DeskyNavHost`, 4 screens, CameraX/ML Kit scan delegating parsing to shared `QrCodeParser`) and SwiftUI (4 views with `ObservableObject` wrappers plus `for await` over SKIE `StateFlow`). Zero business logic. Only imports change.

## 4. Target Structure

```text
client/shared/src/commonMain/kotlin/com/desky/shared/
├── core/                     # SHARED KERNEL (see §5 for naming note)
│                             # protocol, crypto, counters, transport + repo interfaces,
│                             # Room impls, Ipv4, socket expect factory
├── di/                       # kernelModule + one module per feature (see §7)
└── features/
    ├── pairing/              # TODAY flat: ViewModel + UiState + intents + pair/forget use cases
    ├── control/              # TODAY flat, then decomposed: control VM + sender merged +
    │                         # telemetry + alive + disconnect + manual + expression
    ├── settings/             # TODAY flat: SettingsViewModel + device config (or direct repo calls)
    └── session/              # SessionGateViewModel (cross-cutting fence)
```

Reserved-but-empty internal slots (do not create until graduation triggers in §5 fire):

```text
features/<name>/presentation/
features/<name>/domain/
features/<name>/data/
```

Test mirror:

```text
commonTest/kotlin/com/desky/shared/
├── core/                     # kernel tests (codecs, crypto, counters, repos, Ipv4)
└── features/
    ├── pairing/
    ├── control/
    ├── settings/
    └── session/
```

### Kernel naming: `core/` (decided, Phase 0)

The shared kernel lives in the single `com.desky.shared.core` package (`core/db/` for Room database/DAOs/entities). The old `domain/` plus `data/` split is gone: Phase 0 moved every kernel file plus its test verbatim into `core/` and swept all imports. No behavior change; SKIE-visible signatures, Room schema, Koin scopes, and platform actuals are untouched.

## 5. Packaging Rules (Proposed AGENTS.md Update)

1. Kernel owns cross-feature state: protocol codecs and models, `Outcome`, crypto, counters, transport and vault boundaries, repository interfaces, Room database/DAOs/entities and repository impls, IPv4 helpers, socket factory contract.
2. Features own screen state plus private orchestration: ViewModel, UiState, intents, and use cases no other feature calls. Feature-private models stay in the feature folder.
3. Never duplicate kernel types into features. Never place feature-specific logic in the kernel.
4. Internal `presentation/`, `domain/`, `data/` subpackages under a feature are allowed but never required upfront.
5. Graduation triggers (create internal subpackages only when one fires):
   - The feature gains a second independent data source.
   - A repository interface is used only by that feature.
   - 3-plus related use cases grow shared helpers worth isolating.
   - A dedicated platform actual (binder, store, driver) belongs to that feature alone.
6. One Koin module per feature plus kernel module(s). New features add a module, never edit others to land.
7. Tests mirror source packages. Shared doubles live with the package they double.
8. Gotchas-only comments, no history lessons, no breadcrumbs, no dead code (existing hygiene rules unchanged).
9. Any `commonMain` public signature change affecting iOS requires `:shared:syncFramework` verification.

## 6. File Disposition

| File today | Disposition |
|---|---|
| core/Models, Codec, Outcome, UdpDatagram, UdpSocketManager, UdpSocketBinder, PairSecretStore, DeviceRepository, SettingsRepository, Sha256, PairingCrypto, QrCodeParser, ToRobotCounter, FromRobotCounter | Kernel, already in `core/` after Phase 0 |
| domain/PairDeviceWithCodeUseCase, PairRefreshUseCase, ForgetDeviceUseCase | Move to `features/pairing/` (Forget is initiated from pairing/settings/control; home it in pairing and inject where needed) |
| domain/GetPairPskUseCase | Move to `features/pairing/` first; fold into session/device helper only after caller audit |
| domain/SendSignedControlUseCase + EmergencyStopUseCase | Merge into one `features/control/` sender file, merge tests |
| domain/ObserveTelemetryUseCase, AliveUseCase, ObserveRemoteDisconnectUseCase, ManualModeUseCases, ExpressionUseCases | Move to `features/control/` |
| domain/DeviceConfigUseCases | Move to `features/settings/` first; inline to `SettingsRepository` calls only after caller audit plus test move |
| presentation/PairingViewModel | Move to `features/pairing/` |
| presentation/RobotControlViewModel | Decompose (see §8) then move to `features/control/` |
| presentation/SettingsViewModel | Move to `features/settings/` |
| presentation/SessionGateViewModel | Move to `features/session/` |
| data/*, data/db/*, androidMain/*, iosMain/*, jvmMain/* | No moves, import updates only |
| WifiRejoinMonitor.swift, ScanQrView/CameraX scan shells | No moves, import updates only |

## 7. DI Changes

Replace the single `domainModule` plus `presentationModule` split with kernel plus per-feature modules. Suggested shape (names illustrative):

- `kernelModule`: counters, repositories, socket manager, vault, ioScope.
- `pairingModule`: PairWithCode, GetPairPsk (until folded), Forget, PairingViewModel.
- `controlModule`: sender, telemetry, alive, disconnect, manual, expression, refresh trigger, RobotControlViewModel.
- `settingsModule`: device config (until folded), SettingsViewModel.
- `sessionModule`: SessionGateViewModel.

Preserve `getOrNull` optional bindings for binder, alive, refresh, disconnect, manual, and expression so JVM/iOS graphs without Wi-Fi binder keep resolving. Update `IosKoin.kt` imports and typed accessors in the same commit as the move. Keep Android per-destination factory scope and iOS app-lifetime `SharedGraph` caching unchanged.

## 8. RobotControlViewModel Decomposition (Required, Not Optional)

Moving the 1193-line file changes nothing. Before or alongside its move, split responsibilities behind internal boundaries inside `features/control/`:

- Drive-loop ticker (joystick state to 20 ms tick to `send`), idle slowdown, failsafe heartbeat invariant.
- Telemetry collector (socket flow through decode to UiState, timeout constants).
- ALIVE plus rejoin cadence (`networkAvailable` / `onForeground` knock handling).
- Mode gating (MANUAL take/release, e-stop latch, unpair/refresh/disconnect side effects).

Keep one public ViewModel with one UiState. Extract private helpers or internal classes with unit tests first, then relocate. Add coverage in `RobotControlViewModelTest` plus new helper tests before the move so the move itself is import-only.

## 9. Test Migration

- Move each test with its source in the same commit. Merge `SendControlUseCasesTest` halves when senders merge.
- `TestDoubles.kt` follows the packages it doubles; split only if it grows feature-specific fakes.
- Room and IPv4 tests stay with the kernel.
- No coverage gate changes (Kover is report-only). Do not set verification bounds during the move.

## 10. iOS, SKIE, and Platform Constraints

- Keep all shared suspend functions top-level. Never move crypto or use-case suspend members into `object` singletons.
- After each slice: run `:shared:syncFramework`, rebuild the pod, confirm the 4 Swift wrapper views plus `SharedGraph` plus `SessionGateState` still compile.
- Keep the `iosApp/Kotlin` to `iosMain` `srcDir` wiring untouched.
- Keep `COCOAPODS_SKIP_KOTLIN_BUILD` workflow noted in ARCHITECTURE.md for pure-Swift iteration.

## 11. Build, Lint, and Baseline Handling

Gating today: Android Lint (baseline), detekt (baseline), baseline no-growth guard. Advisory: ktlint, Kover, OSV, iOS analyze/lint/format.

Mass moves churn baselines. Per ARCHITECTURE.md § promotion and regen rules:

- Regenerate baselines only with their tools (`updateLintBaseline`, `detektBaseline`), review the diff, commit.
- Never hand-edit baseline IDs. Never silently bump `tools/baseline-guard.sh` caps; each bump is its own reviewed change with the regenerated baseline.
- Per-slice verification: `jvmTest`, `assemble`, `syncFramework`, `make check` (or at minimum lint plus detekt plus baseline-guard). Full `./gradlew :androidApp:assembleDebug` plus iOS device check on the control slice for the rejoin knock.

## 12. Doc Updates Included in This Plan

- `client/AGENTS.md`: replace flat-ban wording with kernel-plus-slices rules from §5, including graduation triggers and per-feature Koin module rule.
- `client/ARCHITECTURE.md`: replace aspirational `features/` tree with §4 tree, record `core/` kernel decision, add per-feature module ownership note, fix `shared/protocol/README.md` link (actual spec lives at repo-root `shared/protocol/`, not under `client/shared`).
- This file remains the record of why full hybrid was deferred and what triggers it.

## 13. Phased Execution

- Phase 0 (docs): update AGENTS.md plus ARCHITECTURE.md per §12. No code moves. Verify docs build/links only.
- Phase 1 (settings pilot, smallest blast radius): move SettingsViewModel plus DeviceConfig, add `settingsModule`, update AppModules plus IosKoin plus Android/iOS settings screens, move settings tests. Verify per §11.
- Phase 2 (pairing): move PairingViewModel plus pair/forget/PSK use cases, add `pairingModule`, update QR scan callers plus pair/unpair screens on both shells, move pairing tests.
- Phase 3 (session): move SessionGateViewModel, add `sessionModule`, update NavHost fence plus Swift gate state, move gate test.
- Phase 4 (control, largest): decompose per §8 with new helper tests, then move control use cases plus ViewModel, add `controlModule`, update control screens plus rejoin wiring, move control tests. Include iOS device check for Wi-Fi rejoin.
- Phase 5 (folding, optional): only after all slices land and callers are audited, fold GetPairPsk and DeviceConfig one-liners and delete the vacated tests/files. Separate commits per fold.
- Phase 6 (baselines): regen lint/detekt baselines as a standalone reviewed change if the guard complains.

Order matters: settings first proves the pattern cheaply, control last because it carries the drive loop and most platform risk.

## 14. Risks

- Import churn across shells and tests is the main cost. Mitigated by one slice per commit plus per-slice verification.
- Accidental SKIE breakage from regrouping suspend functions. Mitigated by top-level-only rule plus `syncFramework` per slice.
- Koin graph breakage on JVM/iOS without binder. Mitigated by preserving `getOrNull` optionals and testing the binderless graph.
- Baseline-guard failures blocking CI during moves. Mitigated by treating regen as its own reviewed change, not smuggled into feature commits.
- Premature layering inside features. Mitigated by graduation triggers in §5; reviewers should reject `presentation/`/`domain/`/`data/` scaffolding with one file in each.
