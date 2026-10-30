package com.desky.shared.core

import android.content.Context
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKeys
import com.desky.shared.core.PairSecretStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Value key inside each per-device preferences file. */
private const val PSK_VALUE_KEY = "psk_hex"

/**
 * Android [PairSecretStore]: one `EncryptedSharedPreferences` file per
 * device (`bond_<sanitized deviceId>`), `MasterKeys` AES256_GCM key with key
 * encryption AES256_SIV + value encryption AES256_GCM
 * (`androidx.security:security-crypto:1.0.0` API).
 *
 * Missing entries read back null (never throw); [clear] removes the value
 * (no-op when absent). Key material is never logged.
 */
class EncryptedPairSecretStore(
    context: Context,
) : PairSecretStore {
    private val appContext = context.applicationContext

    // HOLD: MasterKeys.getOrCreate (security-crypto 1.0.0) is deprecated, but
    // so is its replacement MasterKey — the whole security-crypto line is
    // deprecated upstream. The real fix is a vault rewrite with migration
    // (separate task); pre-ship there are no users to migrate, so hold.
    @Suppress("DEPRECATION")
    private fun prefsFor(deviceId: String) =
        EncryptedSharedPreferences.create(
            "bond_${deviceId.sanitized()}",
            MasterKeys.getOrCreate(MasterKeys.AES256_GCM_SPEC),
            appContext,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
        )

    // commit() (blocking, already on Dispatchers.IO) instead of apply()
    // (async): a crash/power loss between apply()'s in-memory write and its
    // async flush must not lose the freshly-paired pair PSK.
    override suspend fun savePsk(deviceId: String, pskHex: String) {
        withContext(Dispatchers.IO) {
            prefsFor(deviceId).edit().putString(PSK_VALUE_KEY, pskHex).commit()
        }
    }

    override suspend fun loadPsk(deviceId: String): String? =
        withContext(Dispatchers.IO) {
            prefsFor(deviceId).getString(PSK_VALUE_KEY, null)
        }

    override suspend fun clear(deviceId: String) {
        withContext(Dispatchers.IO) {
            prefsFor(deviceId).edit().remove(PSK_VALUE_KEY).commit()
        }
    }

    /** File-name safe: keep alphanumerics plus `._-`, fold the rest to `_`. */
    private fun String.sanitized(): String =
        map { c -> if (c.isLetterOrDigit() || c == '.' || c == '_' || c == '-') c else '_' }
            .joinToString("")
}

/**
 * Android factory: the caller (see `DeskyApp`) supplies the Context, matching
 * the existing `createDeskyDatabase(context)` DI pattern.
 */
fun createPairSecretStore(context: Context): PairSecretStore =
    EncryptedPairSecretStore(context)
