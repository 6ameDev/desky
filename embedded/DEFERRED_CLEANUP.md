# Deferred Cleanup Ledger (Step 2)

Step 1 renamed code identifiers only; the items below change what is
sent, stored, or asserted, so they move together in one grouped pass.

Rule: change once across ALL actors at the same time — firmware NVS,
both firmwares' wire codecs, the future mobile client, bench scripts,
and test goldens. Never rename one side alone.

## Frozen items

1. NVS key strings: namespace "desky", "cfg_version", "psk", "bctr",
   "bonded" (kPairedKey), "bera". WHO: shipped-device flash; a rename
   orphans every existing pair and forces re-pair.
2. Wire opcodes REKEY_REQ / REKEY_ACK / UNBOND (0x53/0x54/0xAD) and the
   PAIR family (0x50/0x51/0x52). WHO: robot RX demux, mobile client,
   bench dissectors, test goldens, shared/protocol/README.md.
3. Constants kRekey* / kUnbond* (headers, sizes, tag/message lengths).
   WHO: same actors as (2); they pin the frozen frame shapes in code.
4. Log-line bond vocabulary ("bond loaded", "unbond ok", "rekey ok",
   drop strings, NVS failure strings). WHO: bench log scrapers and
   field-telemetry accounting keyed on these literals.
5. Type/dir names PairingTask, pairing::Core, pairing/ dir, namespace
   pairing, PAIRSHOW/PAIRDONE link types. WHO: cross-firmware docs and
   the head-link contract; rename only with a matching doc update.

## Grouped-pass checklist

Pick the pass, then update firmware + client + bench + goldens +
protocol README in the same change and re-run both gates
(`embedded/core`, `embedded/head`) plus the client suite.
