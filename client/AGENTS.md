# AGENTS.md — Desky Mobile Client (KMP)

> **Critical constraint for subagents:** You have no access to prior session context. Treat every instruction as self-contained. Re-read this file before each task.

## Monorepo Layout & Scope
- `client/shared/` — Core Kotlin Multiplatform module (Domain models, Repositories, Feature ViewModels, Platform actuals).
- `client/androidApp/` — Thin Jetpack Compose UI shell for Android.
- `client/iosApp/` — Thin SwiftUI shell for iOS via CocoaPods + SKIE.

## Architecture & Packaging Rules
1. **Feature-First Packaging:** Never dump files into a flat global `domain/` or `presentation/` directory. Code must be organized by feature modules under `com.desky.shared.features.*` (e.g., `features/pairing/`, `features/control/`, `features/settings/`).
2. **No Micro-Use-Case Bloat:** Do not create a separate file for every single button click. Group related operations or let ViewModels/Repositories interact directly unless complex reusable state coordination is required.
3. **Strict Dependency Flow (Inward Only):** 
   - `androidApp` / `iosApp` depend ONLY on Shared Presentation/Features.
   - Domain layer depends on nothing (pure Kotlin logic).
   - Data layer implements interfaces defined in Domain.
4. **Unidirectional Data Flow (UDF):** Every screen exposes a single immutable `StateFlow<UiState>` and consumes explicit UI intents.

## SKIE & Swift Interop Constraints (House Rules)
- **Top-Level Suspend Functions Only:** All shared `suspend` functions exposed to iOS **must be top-level functions**. Never make a `suspend` function a member of a Kotlin `object` singleton. SKIE 0.10.15 fails to generate valid Swift wrappers for static object suspend members, breaking `syncFramework`.
- **Verify Framework Sync:** Any change to `commonMain` signatures affecting iOS requires running `./gradlew :shared:syncFramework` to ensure Swift compilation succeeds.

## Code Hygiene & Anti-Archaeology
- **Zero History Lessons:** Source files are never change logs. Never write comments explaining what code *used* to do or what spikes were tried.
- **No Task Breadcrumbs:** Never include task numbers, subagent notes, or planner references in source files.
- **Gotchas Only:** Comments are strictly for non-obvious network or concurrency gotchas. Refactor unclear code instead of commenting.
- **No Dead Code Hoarding:** Completely delete replaced code; never leave commented-out blocks or unused compatibility wrappers.
