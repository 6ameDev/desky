package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout

/** Default per-send wait schedule (ms): 1s, 2s, 4s, 4s, 4s (5 sends, 15s total). */
val DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS: List<Long> = listOf(1000, 2000, 4000, 4000, 4000)

/**
 * Pair with the robot at ([ROBOT_HOST]:[DEFAULT_UDP_PORT], resolved via
 * mDNS on the shared home network) using the 6-digit code shown on its
 * display (pairing/auth v1, frozen spec:
 * `shared/protocol/README.md` §"Pairing + authenticated drive v1").
 *
 * Flow: validate the code -> `client_nonce = pairingNonce()` -> send
 * `PAIR_REQ` -> await the first `PAIR_OK` (38B v1 / 39B v1.1, `0x51`) or
 * `PAIR_ERR` (4B/`0x52`) on the shared [UdpSocketManager.incoming] flow
 * within the per-send budget, re-sending `PAIR_REQ` on silence only ->
 * on `PAIR_OK` derive `KEK = HKDF-SHA256(client||server, code,
 * "desky-pair-v1", 16)` and unwrap the pair PSK with AES-128-GCM
 * (`nonce12 = server||client||00^4`, `AAD = 0x51||client||server`) ->
 * persist the pair.
 *
 * The unwrapped PSK is stored in the platform vault ([PairSecretStore]);
 * the Room row carries no key material (Task 9: no `pskHex` column) and
 * `lastCounter = 0` so the drive session's first wire packet uses counter 1.
 * The v1.1 trailing interval byte (when a 39B frame arrives) becomes
 * [PairedDevice.persistEvery] (sanitized 8..1024 else 128; a 38B v1 frame
 * defaults to 128) and is Room-persisted with the row, so the drive
 * session keeps the negotiated cadence across restarts.
 * The returned [PairedDevice] matches the persisted row; session code reads
 * the key via [GetPairPskUseCase].
 *
 * Retry policy: at most [attemptTimeoutsMs.size] `PAIR_REQ` datagrams leave
 * the app per [invoke] (default 5; total 15s, well inside the firmware 60s
 * redelivery window). Retries fire ONLY on silence: ANY reply (`PAIR_OK` or
 * `PAIR_ERR`) terminates the loop immediately. A resend after a lost reply
 * may consume one firmware try, hence sends stay <= 5. The resends are
 * byte-identical (same code+nonce), hence idempotent: a duplicated request
 * the robot already answered just re-triggers the same reply.
 *
 * @param attemptTimeoutsMs per-send wait schedule in ms; sends == size.
 *   Must hold 1..5 positive entries (upper bound mirrors the firmware
 *   5-try lockout so even an all-silent run cannot self-trip it).
 * @param onProgress plain progress callback invoked synchronously before
 *   each send as `(attempt 1..N, maxSends N)`.
 */
class PairDeviceWithCodeUseCase(
    private val deviceRepo: DeviceRepository,
    private val udp: UdpSocketManager,
    private val secretStore: PairSecretStore,
    private val fromRobotCounter: FromRobotCounter,
    private val attemptTimeoutsMs: List<Long> = DEFAULT_PAIR_ATTEMPT_TIMEOUTS_MS,
) {
    init {
        require(attemptTimeoutsMs.size in 1..5) {
            "attemptTimeoutsMs must hold 1..5 entries (got ${attemptTimeoutsMs.size})"
        }
        require(attemptTimeoutsMs.all { it > 0 }) {
            "attemptTimeoutsMs entries must all be > 0"
        }
    }

    suspend operator fun invoke(
        code: String,
        onProgress: (attempt: Int, maxSends: Int) -> Unit = { _, _ -> },
    ): Outcome<PairedDevice> {
        val trimmed = code.trim()
        val codeFailure = validateCode(trimmed)
        if (codeFailure != null) return codeFailure
        val codeBytes = trimmed.encodeToByteArray()
        val clientNonce = pairingNonce()
        val req = PairingPacketCodec.encodePairReq(codeBytes, clientNonce)
        return try {
            // Up to N sends total, SAME req bytes every time: harder retries
            // risk the firmware lockout, and the user can tap Pair again.
            // Retry ONLY on silence — any reply (OK or ERR) breaks immediately.
            var reply: UdpDatagram? = null
            for (i in attemptTimeoutsMs.indices) {
                onProgress(i + 1, attemptTimeoutsMs.size)
                udp.send(req, ROBOT_HOST, DEFAULT_UDP_PORT)
                reply = awaitPairingReply(attemptTimeoutsMs[i])
                if (reply != null) break
            }
            val r = reply ?: return Outcome.Failure("No response from robot")
            handleReply(r.bytes, codeBytes, clientNonce)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to send pairing request: ${e.message}", e)
        }
    }

    private fun validateCode(code: String): Outcome.Failure? {
        if (code.length != PAIR_CODE_SIZE) {
            return Outcome.Failure("Code must be exactly 6 digits (got ${code.length})")
        }
        if (!code.all { it in '0'..'9' }) {
            return Outcome.Failure("Code must contain ASCII digits only")
        }
        return null
    }

    /** First incoming datagram shaped like a pairing reply, or null on timeout. */
    private suspend fun awaitPairingReply(timeoutMs: Long): UdpDatagram? {
        return try {
            withTimeout(timeoutMs) {
                udp.incoming.first { isPairingReply(it.bytes) }
            }
        } catch (e: TimeoutCancellationException) {
            null
        }
    }

    private fun isPairingReply(bytes: ByteArray): Boolean {
        if (bytes.isEmpty()) return false
        val header = bytes[0].toInt() and 0xFF
        return ((bytes.size == PAIR_OK_SIZE || bytes.size == PAIR_OK_SIZE_V11) && header == PAIR_OK_HEADER) ||
            (bytes.size == PAIR_ERR_SIZE && header == PAIR_ERR_HEADER)
    }

    private suspend fun handleReply(
        reply: ByteArray,
        code: ByteArray,
        clientNonce: ByteArray,
    ): Outcome<PairedDevice> {
        if (reply.size == PAIR_ERR_SIZE) {
            val err = PairingPacketCodec.decodePairErr(reply)
                ?: return Outcome.Failure("Pairing failed: invalid response from robot")
            val (reason, retryS) = err
            return Outcome.Failure(mapPairError(reason, retryS))
        }
        // Sized/filtered as PAIR_OK above (38B v1 or 39B v1.1); a bad XOR
        // here means corruption.
        val ok = PairingPacketCodec.decodePairOk(reply)
            ?: return Outcome.Failure("Pairing failed: invalid response from robot")
        val (serverNonce, ct, tag) = ok
        val kek = hkdfSha256(
            salt = clientNonce + serverNonce,
            ikm = code,
            info = "desky-pair-v1",
            length = 16,
        )
        val nonce12 = serverNonce + clientNonce + ByteArray(4)
        val aad = byteArrayOf(PAIR_OK_HEADER.toByte()) + clientNonce + serverNonce
        val (psk, persistEvery) = try {
            unwrapPairPlaintext(aesGcmDecrypt(kek, nonce12, aad, ct, tag))
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            return Outcome.Failure("Pairing failed: could not unwrap key", e)
        }
        val device = PairedDevice(
            id = "desky-$ROBOT_HOST",
            ip = ROBOT_HOST,
            port = DEFAULT_UDP_PORT,
            authToken = "psk-v1",
            // Fresh pair: the counter restarts so the first drive packet is 1.
            lastCounter = 0u,
            alias = ROBOT_HOST,
            persistEvery = persistEvery,
        )
        return try {
            // Vault first: if the Room write below throws, the key already
            // rests encrypted and the next pairing overwrites it. (Reversing
            // the order would risk the worse outcome — a persisted row whose
            // key is lost.)
            secretStore.savePsk(device.id, psk.toHex())
            deviceRepo.save(device)
            // Fresh pair: the robot restarts its own send sequence at 1, so
            // the receive domain returns to the 0 baseline (the peer id is
            // stable across pairs — only an explicit reset does this).
            fromRobotCounter.reset()
            Outcome.Success(device)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Outcome.Failure("Failed to persist paired device: ${e.message}", e)
        }
    }

    private fun mapPairError(reason: Int, retryS: Int): String = when (reason) {
        0 -> "Pairing failed: incorrect code"
        1 -> "Pairing failed: robot locked, retry in ${retryS}s"
        // Reason 2 is genuine window expiry (tryCode != kOk); the single-use
        // code is spent, so pair again with the fresh display code.
        2 -> "Pairing failed: code expired"
        3 -> "Pairing failed: robot already paired"
        // M11 additive reason 4 (internal_retry_fresh): NVS/crypto failure
        // after tryCode kOk (code already burned) — distinct from expired.
        4 -> "Pairing failed: robot error, try a fresh code"
        // Unknown reasons (incl. old-app × new-robot reason 4): generic,
        // non-retryable (any PAIR_ERR terminates the send loop), raw int
        // preserved for logs.
        else -> "Pairing failed: robot rejected pairing (reason=$reason)"
    }
}
