# Desky

> **Critical constraint for subagents:** You have no access to prior session context. Treat every instruction as self-contained. Re-read this file before each task.

## Monorepo Layout & Routing
- `embedded/` — Embedded firmwares (`core/`, `head/`) and shared contracts. **If your task touches `embedded/`, you MUST read `embedded/AGENTS.md` before writing code.**
- `client/` — Placeholder for the future KMP / Compose mobile app.
- `hardware/` — Hand-solder carriers and physical contracts. Read `hardware/INTERFACE.md` §0 first if touching this directory.
- `shared/` — Cross-firmware shared schemas, pure functions, and protocol definitions.

## Global Code Hygiene & Anti-Archaeology Policy
Applies to every directory (`embedded/`, `client/`, `shared/`):
- **Zero History Lessons:** Source files are never change logs. Never write comments explaining what code *used* to do or what spikes were tried.
- **No Task Breadcrumbs:** Never include task numbers, subagent notes, or planner references in source files.
- **The Self-Documenting Rule:** Comments are strictly reserved for non-obvious constraints, race condition warnings, or critical timing gotchas. Refactor unclear code instead of commenting.
- **No Dead Code Hoarding:** When refactoring, completely remove replaced code. Never leave commented-out blocks or unused "backwards compatibility" wrappers.

## General Subagent Rules
- Prefer existing `make` targets and test gates over raw tool commands.
- All new cross-seam contracts must be pinned with host tests in local test suites.
- Respect monorepo boundaries: never mix mobile app logic into embedded firmwares or vice-versa.
