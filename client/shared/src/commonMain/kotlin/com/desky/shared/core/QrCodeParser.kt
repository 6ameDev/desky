package com.desky.shared.core

/**
 * QR code payload parser for the pairing-code shortcut.
 *
 * Scan is a code-entry shortcut: a decoded QR payload that is exactly the
 * 6-digit pairing code feeds the identical [PairDeviceWithCodeUseCase] path
 * as the manual field. Only the bare form is accepted today:
 * trim surrounding whitespace, then require `^\d{6}$` (ASCII digits only).
 *
 * URL / deep-link forms (e.g. `desky://pair?code=123456` or an
 * `https://…` link carrying the code) are deliberately NOT accepted and
 * return null — they are reserved for a later task. Callers surface the
 * shared "not a Desky code" error for those.
 *
 * NORMATIVE PAYLOAD CONTRACT: shared/protocol/README.md §"Pairing QR" —
 * this parser and firmware `middleware/qr_pairing.h` must agree (bare 6
 * digits, numeric mode, V1 minimum, ECC-LOW); never extend the payload
 * on one side alone.
 */
object QrCodeParser {
    private val BareCode = Regex("^[0-9]{6}$")

    /**
     * Returns the 6-digit code, or null when [raw] is not a bare Desky code.
     */
    fun parse(raw: String): String? {
        val trimmed = raw.trim()
        if (!BareCode.matches(trimmed)) return null
        return trimmed
    }
}
