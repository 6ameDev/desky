package com.desky.shared.core

/**
 * Platform secure vault for pair PSKs (pairing/auth Task 8, vault-only
 * since Task 9).
 *
 * The vault is the ONLY at-rest home of the 16-byte pair secret (lowercase
 * hex): Room's `paired_device` table carries the drive counter alone and
 * never key material. [GetPairPskUseCase] is the single session-load entry
 * point.
 *
 * Semantics: [loadPsk] returns null when nothing is stored (never throws
 * for a missing entry); [savePsk] overwrites; [clear] removes the entry and
 * is a no-op when absent. Implementations MUST NEVER log key material.
 *
 * The vault is at-rest storage only: Task 9's send loop calls
 * [GetPairPskUseCase] once at startup and caches the key in memory — it
 * must never read the vault per tick.
 *
 * Factory note (deliberate deviation from a no-arg `expect`): Android needs
 * a `Context` to build `EncryptedSharedPreferences`, and there is no
 * process-global holder for it — the existing DI pattern threads
 * Context-built dependencies in as `dataModule` parameters (see
 * `createDeskyDatabase(context)` in `DeskyApp`). So each platform exposes
 * its own `createPairSecretStore` factory instead of an expect/actual pair:
 * - androidMain: `createPairSecretStore(context: Context)` (EncryptedSharedPreferences)
 * - iosMain: `createPairSecretStore()` (Keychain Services)
 * - jvmMain: `createPairSecretStore()` (in-memory; non-production)
 * and `dataModule(db, bondSecretStore)` registers the built instance as a
 * `PairSecretStore` singleton. Common code depends only on this interface.
 */
interface PairSecretStore {
    /** Persist (overwriting) the pair PSK hex for [deviceId]. */
    suspend fun savePsk(deviceId: String, pskHex: String)

    /** Stored PSK hex for [deviceId], or null when absent. Never throws for missing. */
    suspend fun loadPsk(deviceId: String): String?

    /** Remove the entry for [deviceId]; no-op when absent. */
    suspend fun clear(deviceId: String)
}
