# Expression Ability — Layered Build Plan

The expression ability is the robot's expressive output channel. Callers
request an expression by id. The ability plays the bundle: eye animation
on the head display, and later an audio cue on the speaker, a motion
primitive on the body, a head pose, an arm gesture, and a log line.

Each layer executes on top of the previous one. Later work arrives as new
table rows, never as rewrites.

## As-built notes (modes-expressions branch)

- The screen is owned by the display ability (abilities/display), which
  arbitrates pairing, QR, OTA, and eyes slots. Expression state (request
  ids, priority table, preemption) lives alongside it and will split into
  abilities/expression/ when sound and motion fan-out land.
- V1 is eyes-only. Audio cues, motion primitives, head poses, and arm
  gestures are bundle fields reserved for V2 and V3, not yet played.
- System modes are NORMAL, MANUAL, PAIRING, and OTA with the failsafe as
  an overlay. The display follows the mode: pairing and OTA own the
  screen while held, expressions own it otherwise. Manual driving needs
  an explicit take action; paired no longer implies drivable.
- The display stack is u8g2-only behind the hal driver interface. Eyes
  render from a vendored parametric eye-animation library with random
  emotion selection off and auto-blink plus random look on.
- Core owns the catalog and dispatches by id over the link; head maps to
  presets with neutral fallback and keeps screen arbitration. The shared
  table is the single source both sides read.
- The app picker carries a hardcoded list mirroring the catalog and plays
  through core MANUAL-gated triggers. The list-fetch round trip is retired;
  headers stay reserved.

## Layer rules (apply to all versions)

- Codec holds ids, the bundle struct, and the tables. The manager holds
  the active id, the priority table, and preemption policy. The task
  plays bundles and is the only layer touching drivers.
- One active expression at a time with priority preemption. Blending is
  out of scope at every layer.
- Every expression resolves back to neutral, the explicit baseline state.
- Callers reference face, audio, and motion by id. No inline driver
  calls in tables or policies.
- Every layer ships with its host tests and a bench proof before merge.

## V1 — Foundation plus five entries (BUILT)

- Entries: neutral, happy, sad, curious, surprised, plus the holding
  alert row kept for OTA failure. Request ids append without renumbering.
- Surprised is the first play-once micro-reaction with a 1500ms hold.
  The rest hold until preempted.
- Triggers are the existing CLI verb, link frames, core dispatch, and the
  MANUAL-gated app picker. No autonomy.
- Host tests pin the vocabulary, the table rows, the preemption order,
  play-once release, and trigger paths for all five ids.
- Bench proof: play all five, demo a high-priority interrupt over a low
  one, confirm the yield back to neutral.

## V2 — Sound, motion, and high-use entries

- Bundle fan-out grows to audio cues and motion primitives plus head and
  arm poses. Core ability and dispatch already exist; the remaining split
  is head-side at fan-out time, with the display ability keeping screen
  arbitration only.
- New rows: angry, scared, excited, confused. Each row arrives with its
  full bundle fields and its priority slot. Priority order across the
  catalog locks here.
- First behavior hookup: safety modules request entries by id through
  the same path the CLI uses today.
- Host tests extend the table and preemption pins. Bench proof plays
  each new entry with its sound and motion together.

## V3 — Full catalog plus polish

- New rows: playful, triumphant, sleepy, content, determined.
- Sleepy and content introduce the low-energy sustained moods for long
  idle and rest. Triumphant and playful cover goal completion and
  attention-seeking moments.
- Polish items: per-entry intensity as an optional request parameter
  defaulting to normal, and timing tuning from bench observation
  (durations, interrupt windows).
- Host tests cover new rows and intensity defaults. Bench proof walks
  the full catalog in one session.

## Open questions

- Priority order across the full catalog locks in V2 — V1 order is
  surprised, alert, sad, curious, happy, neutral.
- Intensity in V3 or deferred further?
