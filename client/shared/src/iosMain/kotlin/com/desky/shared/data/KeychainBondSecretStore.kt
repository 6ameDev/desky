package com.desky.shared.data

import com.desky.shared.domain.BondSecretStore
import kotlinx.cinterop.COpaquePointerVar
import kotlinx.cinterop.ExperimentalForeignApi
import kotlinx.cinterop.UByteVar
import kotlinx.cinterop.addressOf
import kotlinx.cinterop.alloc
import kotlinx.cinterop.convert
import kotlinx.cinterop.memScoped
import kotlinx.cinterop.ptr
import kotlinx.cinterop.reinterpret
import kotlinx.cinterop.usePinned
import kotlinx.cinterop.value
import platform.CoreFoundation.CFDataCreate
import platform.CoreFoundation.CFDataGetBytePtr
import platform.CoreFoundation.CFDataGetLength
import platform.CoreFoundation.CFDataRef
import platform.CoreFoundation.CFDictionaryCreateMutable
import platform.CoreFoundation.CFDictionarySetValue
import platform.CoreFoundation.CFRelease
import platform.CoreFoundation.CFStringCreateWithCString
import platform.CoreFoundation.CFStringRef
import platform.CoreFoundation.kCFBooleanTrue
import platform.CoreFoundation.kCFStringEncodingUTF8
import platform.Security.SecItemAdd
import platform.Security.SecItemCopyMatching
import platform.Security.SecItemDelete
import platform.Security.SecItemUpdate
import platform.Security.errSecDuplicateItem
import platform.Security.errSecSuccess
import platform.Security.kSecAttrAccessible
import platform.Security.kSecAttrAccessibleWhenUnlocked
import platform.Security.kSecAttrAccount
import platform.Security.kSecAttrService
import platform.Security.kSecClass
import platform.Security.kSecClassGenericPassword
import platform.Security.kSecMatchLimit
import platform.Security.kSecMatchLimitOne
import platform.Security.kSecReturnData
import platform.Security.kSecValueData
import platform.posix.memcpy

/** Keychain service grouping all desky bond entries. */
private const val BOND_SERVICE = "com.desky.bond"

/**
 * iOS [BondSecretStore]: Keychain Services generic-password entries, one per
 * device (`service = "com.desky.bond"`, `account = deviceId`), readable only
 * while the device is unlocked (`kSecAttrAccessibleWhenUnlocked`).
 *
 * [loadPsk] returns null when the entry is missing (or unreadable) and never
 * throws for a missing entry; [clear] deletes and ignores absent entries.
 * Key material is never logged.
 */
@OptIn(ExperimentalForeignApi::class)
class KeychainBondSecretStore : BondSecretStore {

    override suspend fun savePsk(deviceId: String, pskHex: String) {
        memScoped {
            val service = cfString(BOND_SERVICE)
            val account = cfString(deviceId)
            val data = cfData(pskHex.encodeToByteArray())
            try {
                var status = SecItemAdd(newQuery(service, account, data), null)
                if (status == errSecDuplicateItem) {
                    val attrs = CFDictionaryCreateMutable(null, 0, null, null)!!
                    CFDictionarySetValue(attrs, kSecValueData, data)
                    CFDictionarySetValue(attrs, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked)
                    status = SecItemUpdate(baseQuery(service, account), attrs)
                }
                if (status != errSecSuccess) {
                    throw IllegalStateException("Keychain save failed (status=$status)")
                }
            } finally {
                if (service != null) CFRelease(service)
                if (account != null) CFRelease(account)
                if (data != null) CFRelease(data)
            }
        }
    }

    override suspend fun loadPsk(deviceId: String): String? {
        memScoped {
            val service = cfString(BOND_SERVICE)
            val account = cfString(deviceId)
            try {
                val query = baseQuery(service, account)
                CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue)
                CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne)
                // Out-param shape must match SecItemCopyMatching's
                // `CPointer<CPointerVar<CFTypeRef>>` exactly, but that
                // pointee (a projected generic pointer) cannot be spelled
                // as a type argument — the bound checker rejects it.
                // Allocate a pointer-sized opaque slot instead and
                // reinterpret at the call (target inferred from the
                // parameter); the returned reference is just a pointer.
                val result = alloc<COpaquePointerVar>()
                if (SecItemCopyMatching(query, result.ptr.reinterpret()) != errSecSuccess) return null
                val data = (result.value as CFDataRef?) ?: return null
                val length = CFDataGetLength(data).toInt()
                if (length == 0) return null
                val bytes = ByteArray(length)
                bytes.usePinned { pinned ->
                    memcpy(pinned.addressOf(0), CFDataGetBytePtr(data), length.convert())
                }
                CFRelease(data)
                return bytes.decodeToString()
            } finally {
                if (service != null) CFRelease(service)
                if (account != null) CFRelease(account)
            }
        }
    }

    override suspend fun clear(deviceId: String) {
        memScoped {
            val service = cfString(BOND_SERVICE)
            val account = cfString(deviceId)
            try {
                SecItemDelete(baseQuery(service, account))
            } finally {
                if (service != null) CFRelease(service)
                if (account != null) CFRelease(account)
            }
        }
    }

    /**
     * Class + service + account selector shared by all four operations.
     * Return type is inferred (the CF dictionary struct tag is not exposed
     * to Kotlin); every caller passes it straight back into CoreFoundation.
     */
    private fun baseQuery(service: CFStringRef?, account: CFStringRef?) =
        CFDictionaryCreateMutable(null, 0, null, null)!!.apply {
            CFDictionarySetValue(this, kSecClass, kSecClassGenericPassword)
            CFDictionarySetValue(this, kSecAttrService, service)
            CFDictionarySetValue(this, kSecAttrAccount, account)
        }

    /** Base selector plus the (unlock-only) value for inserts. */
    private fun newQuery(service: CFStringRef?, account: CFStringRef?, withValue: CFDataRef?) =
        baseQuery(service, account).apply {
            CFDictionarySetValue(this, kSecValueData, withValue)
            CFDictionarySetValue(this, kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked)
        }

    private fun cfString(value: String): CFStringRef? =
        CFStringCreateWithCString(null, value, kCFStringEncodingUTF8)

    private fun cfData(bytes: ByteArray): CFDataRef? =
        bytes.usePinned { pinned ->
            CFDataCreate(null, pinned.addressOf(0).reinterpret<UByteVar>(), bytes.size.toLong())
        }
}

/** iOS factory: Keychain needs no caller-supplied dependencies. */
fun createBondSecretStore(): BondSecretStore = KeychainBondSecretStore()
