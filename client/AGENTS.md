# AGENTS.md — Desky Mobile Client (KMP)

> **Critical constraint for subagents:** You have no access to prior session context. Treat every instruction as self-contained. Re-read this file before each task.

## Monorepo Layout & Scope
- `client/shared/` — Core Kotlin Multiplatform module: shared kernel (`core/`), feature slices (`features/`), DI (`di/`), platform actuals.
- `client/androidApp/` — Thin Jetpack Compose UI shell for Android.
- `client/iosApp/` — Thin SwiftUI shell for iOS via CocoaPods + SKIE.

## Architecture & Packaging Rules
1. **Slices Over Shared Kernel:** Cross-feature code (protocol codecs/models, crypto, counters, transport/vault boundaries, repository interfaces, Room impls) lives in `com.desky.shared.core`. Screen state plus private orchestration lives in `com.desky.shared.features.*` (`pairing/`, `control/`, `settings/`, `session/`). Never duplicate kernel types into features.
2. **No Micro-Use-Case Bloat:** Do not create a separate file for every single button click. Group related operations or let ViewModels/Repositories interact directly unless complex reusable state coordination is required. Merges happen only after a caller audit.
3. **Internal Layers Are Optional:** `features/<name>/presentation|domain|data` subpackages are allowed but never required upfront. Create them only when a feature gains a second data source, a feature-private repository, 3-plus related use cases with shared helpers, or a dedicated platform actual.
4. **Strict Dependency Flow (Inward Only):**
   - `androidApp` / `iosApp` depend ONLY on Shared Presentation/Features.
   - Domain layer depends on nothing (pure Kotlin logic).
   - Data layer implements interfaces defined in Domain.
5. **Unidirectional Data Flow (UDF):** Every screen exposes a single immutable `StateFlow<UiState>` and consumes explicit UI intents.
6. **One Koin Module Per Feature:** Kernel bindings plus `pairingModule`, `controlModule`, `settingsModule`, `sessionModule`. New features add a module instead of editing others to land.

## SKIE & Swift Interop Constraints (House Rules)
- **Top-Level Suspend Functions Only:** All shared `suspend` functions exposed to iOS **must be top-level functions**. Never make a `suspend` function a member of a Kotlin `object` singleton. SKIE 0.10.15 fails to generate valid Swift wrappers for static object suspend members, breaking `syncFramework`.
- **Verify Framework Sync:** Any change to `commonMain` signatures affecting iOS requires running `./gradlew :shared:syncFramework` to ensure Swift compilation succeeds.

## Code Hygiene & Anti-Archaeology
- **Zero History Lessons:** Source files are never change logs. Never write comments explaining what code *used* to do or what spikes were tried.
- **No Task Breadcrumbs:** Never include task numbers, subagent notes, or planner references in source files.
- **Gotchas Only:** Comments are strictly for non-obvious network or concurrency gotchas. Refactor unclear code instead of commenting.
- **No Dead Code Hoarding:** Completely delete replaced code; never leave commented-out blocks or unused compatibility wrappers.
