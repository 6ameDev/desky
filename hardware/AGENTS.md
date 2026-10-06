# AGENTS.md — desky hardware

> Hardware work starts at `INTERFACE.md` §0 — read it before anything else.
> It is normative: build principle, power contract, parked-link rule.
> Everything else under `hardware/` follows it.

- Carriers: `core/desky-core.*`, `head/desky-head.*` (schematic is the source
  of truth; PCB files are placement sketches only — no fab).
- Guides: `make guides` (ERC + netlist + BOM into ignored `build/`).
