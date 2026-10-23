# Expression Ability — Layered Build Plan

The expression ability is the robot's multimodal output channel. Callers
request an expression by id. The ability plays the bundle: face frames on
the head display, an audio cue on the speaker, a motion primitive on the
body, a head pose, an arm gesture, and a log line.

Each layer executes on top of the previous one. Later work arrives as new
table rows, never as rewrites.

## Layer rules (apply to all versions)

- The ability lives in abilities/expression/ with a codec, manager, and
  task split. The codec holds ids, the bundle struct, and the tables.
  The manager holds the active id, the priority table, and preemption
  policy. The task plays bundles and is the only layer touching drivers.
- One active expression at a time with priority preemption. Blending is
  out of scope at every layer.
- Every expression resolves back to neutral, the explicit baseline state.
- Callers reference face, audio, and motion by id. No inline driver
  calls in tables or policies.
- Every layer ships with its host tests and a bench proof before merge.

## V1 — Foundation plus five entries

Goal: five expressions playable by id on request.

- Entries: neutral, happy, sad, curious, surprised. One per expressive
  quadrant plus the baseline, proving the full range early.
- Bundle fields: face frames, audio cue id, motion primitive id, head
  pose, arm gesture, duration, and a hold versus play-once flag.
  Surprised is the first play-once micro-reaction. The rest hold until
  preempted.
- Manager: active id tracking, priority table, preemption rule, neutral
  fallback. Pure logic.
- Task: bundle player fanning out to face, audio, and motion, owning
  timing across loop ticks and reporting completion.
- Triggers are scripted and manual only. This proves the request path
  end to end with no autonomy involved.
- Host tests pin the bundle table, the preemption order, and the
  play-once versus hold rules.
- Bench proof: play all five, demo a high-priority interrupt over a low
  one, confirm face, sound, and motion fire together.
- Exit criteria: five expressions playable by id, preemption correct,
  gates green.

## V2 — High-use entries

Goal: cover the reactions safety and daily use need. Builds on V1.

- New rows: angry (blocked, stalled, pushed), scared (sustained threat,
  retreats), excited (pairing success, reconnects), confused
  (unexpected sensor readings, blocked path).
- No plumbing changes. Each row arrives with its bundle fields and its
  priority slot.
- Host tests extend the table and preemption pins. Bench proof plays
  each new entry plus its interrupt behavior against a holding mood.
- Exit criteria: nine expressions playable, preemption still correct,
  gates green.

## V3 — Full catalog plus polish

Goal: the complete expressive range. Builds on V2.

- New rows: playful, triumphant, sleepy, content, determined.
- Sleepy and content introduce the low-energy sustained moods for long
  idle and rest. Triumphant and playful cover goal completion and
  attention-seeking moments.
- Polish items: per-entry intensity as an optional request parameter
  defaulting to normal, and timing tuning from bench observation
  (durations, interrupt windows).
- Host tests cover new rows and intensity defaults. Bench proof walks
  the full catalog in one session.
- Exit criteria: fourteen expressions playable, intensity supported,
  gates green.

## Open questions

- V1 entry list: neutral, happy, sad, curious, surprised — confirm.
- Priority order across the full catalog — propose with V1, lock by V2.
- Intensity in V3 or deferred further?
