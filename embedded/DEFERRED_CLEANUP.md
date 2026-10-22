# Cleanup Ledger — Step 2 APPLIED (pairing-renaming branch)

Step 1 renamed code identifiers only. Step 2 (this branch) changed what is
sent, stored, and asserted, in one lockstep pass across firmware + client +
bench scripts + goldens + protocol README.

Rule (kept): change once across ALL actors at the same time. Never rename
one side alone.

## Applied

1. NVS: "bonded"->"paired", "bera"->"pera", kConfigVersion 2->3 (auto-clear
   on mismatch). psk/bctr/cfg_version/namespace unchanged. Recovery: repair;
   worst case erase flash + reflash + repair.
2. Opcodes: REKEY_REQ->PAIR_REFRESH_REQ, REKEY_ACK->PAIR_REFRESH_ACK,
   UNBOND->UNPAIR, DRIVE_V2->DRIVE. Values 0x53/0x54/0xAD/0xAC frozen.
3. Tag literals: "REKEY"->"PAIRREFRESH" (msg 11->17B), "REKEYACK"->
   "PAIRREFRESHACK" (msg 10->16B), "UNBOND"->"UNPAIR" (msg stays 10B).
   Frame sizes unchanged (14B/10B/12B/16B).
4. HKDF salt: "desky-rekey-v1"->"desky-pair-refresh-v1". KDF vectors
   regenerated on both sides (next_psk=2ecca11e...).
5. Keychain service: "com.desky.bond"->"com.desky.pair" (app data clear /
   reinstall to recover). Room column `era` unchanged, no migration.
6. Logs: bond/rekey/unbond vocabulary -> pair/pair-refresh/unpair;
   bench scripts moved in the same pass (scripts/ is gitignored local
   tooling — working-tree only, no commit).
7. Client Bond* types -> Pair* (PairSecretStore, GetPairPskUseCase,
   KeychainPairSecretStore, EncryptedPairSecretStore,
   InMemoryPairSecretStore + tests).

## Deliberately unchanged

- ALIVE / DISCONNECT tags + literals (out of scope, owner-confirmed).
- PAIR family opcodes/values, PAIR_OK/PAIR_ERR paths, telemetry 0xBB.
- Type/dir names PairingTask, pairing::Core, pairing/ dir, namespace
  pairing, PAIRSHOW/PAIRDONE link types (architecture vocabulary).
- Head firmware: zero references, zero changes (audited).
- NVS "psk", "bctr", "cfg_version", namespace "desky".
