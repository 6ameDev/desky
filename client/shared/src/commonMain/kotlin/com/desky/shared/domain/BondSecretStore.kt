package com.desky.shared.domain

/**
 * Platform secure vault for bond PSKs (pairing/auth Task 8).
 *
 * The vault is the at-rest home of the 16-byte bond secret (lowercase hex);
 * Room's `paired_device.pskHex` column is legacy (always null for rows
 * written by [PairDeviceWithCodeUseCase], blanked on read by
 * [GetBondPskUseCase]) and is never read anywhere else.
 *
 * Semantics: [loadPsk] returns null when nothing is stored (never throws
 * for a missing entry); [savePsk] overwrites; [clear] removes the entry and
 * is a no-op when absent. Implementations MUST NEVER log key material.
 *
 * The vault is at-rest storage only: Task 9's send loop calls
 * [GetBondPskUseCase] once at startup and caches the key in memory — it
 * must never read the vault per tick.
 *
 * Factory note (deliberate deviation from a no-arg `expect`): Android needs
 * a `Context` to build `EncryptedSharedPreferences`, and there is no
 * process-global holder for it — the existing DI pattern threads
 * Context-built dependencies in as `dataModule` parameters (see
 * `createDeskyDatabase(context)` in `DeskyApp`). So each platform exposes
 * its own `createBondSecretStore` factory instead of an expect/actual pair:
 * - androidMain: `createBondSecretStore(context: Context)` (EncryptedSharedPreferences)
 * - iosMain: `createBondSecretStore()` (Keychain Services)
 * - jvmMain: `createBondSecretStore()` (in-memory; non-production)
 * and `dataModule(db, bondSecretStore)` registers the built instance as a
 * `BondSecretStore` singleton. Common code depends only on this interface.
 */
interface BondSecretStore {
    /** Persist (overwriting) the bond PSK hex for [deviceId]. */
    suspend fun savePsk(deviceId: String, pskHex: String)

    /** Stored PSK hex for [deviceId], or null when absent. Never throws for missing. */
    suspend fun loadPsk(deviceId: String): String?

    /** Remove the entry for [deviceId]; no-op when absent. */
    suspend fun clear(deviceId: String)
}
