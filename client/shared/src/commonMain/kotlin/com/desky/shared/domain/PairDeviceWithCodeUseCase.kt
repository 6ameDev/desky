package com.desky.shared.domain

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeout

/** Per-attempt wait for a pairing reply before a re-send. */
private const val PAIRING_TIMEOUT_MS = 5000L

/** Total `PAIR_REQ` sends per pairing attempt (initial + 2 resends). */
private const val MAX_PAIR_SENDS = 3

/**
 * Pair with the fixed robot (`[ROBOT_HOST]:[DEFAULT_UDP_PORT]`) using the
 * 6-digit code shown on its display (pairing/auth v1, frozen spec:
 * `shared/protocol/README.md` §"Pairing + authenticated drive v1").
 *
 * Flow: validate the code -> `client_nonce = pairingNonce()` -> send
 * `PAIR_REQ` -> await the first `PAIR_OK` (38B/`0x51`) or `PAIR_ERR`
 * (4B/`0x52`) on the shared [UdpSocketManager.incoming] flow within
 * [PAIRING_TIMEOUT_MS], re-sending `PAIR_REQ` up to twice more on timeout ->
 * on `PAIR_OK` derive `KEK = HKDF-SHA256(client||server, code,
 * "desky-pair-v1", 16)` and unwrap the bond PSK with AES-128-GCM
 * (`nonce12 = server||client||00^4`, `AAD = 0x51||client||server`) ->
 * persist the bond.
 *
 * The unwrapped PSK is stored in the platform vault ([BondSecretStore]);
 * the Room row carries no key material (Task 9: no `pskHex` column) and
 * `lastCounter = 0` so the drive session's first wire packet uses counter 1.
 * The returned [PairedDevice] matches the persisted row; session code reads
 * the key via [GetBondPskUseCase].
 *
 * At most 3 `PAIR_REQ` datagrams leave the app per [invoke] by construction
 * (initial + up to 2 resends), so our own retries can never trip the
 * firmware 5-try lockout. The resends are byte-identical (same code+nonce),
 * hence idempotent: a duplicated request the robot already answered just
 * re-triggers the same reply.
 */
class PairDeviceWithCodeUseCase(
    private val deviceRepo: DeviceRepository,
    private val udp: UdpSocketManager,
    private val secretStore: BondSecretStore,
) {
    suspend operator fun invoke(code: String): Outcome<PairedDevice> {
        val trimmed = code.trim()
        val codeFailure = validateCode(trimmed)
        if (codeFailure != null) return codeFailure
        val codeBytes = trimmed.encodeToByteArray()
        val clientNonce = pairingNonce()
        val req = PairingPacketCodec.encodePairReq(codeBytes, clientNonce)
        return try {
            // Up to 3 sends total, SAME req bytes every time: harder retries
            // risk the firmware lockout, and the user can tap Pair again.
            var reply: UdpDatagram? = null
            for (attempt in 0 until MAX_PAIR_SENDS) {
                udp.send(req, ROBOT_HOST, DEFAULT_UDP_PORT)
                reply = awaitPairingReply()
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
    private suspend fun awaitPairingReply(): UdpDatagram? {
        return try {
            withTimeout(PAIRING_TIMEOUT_MS) {
                udp.incoming.first { isPairingReply(it.bytes) }
            }
        } catch (e: TimeoutCancellationException) {
            null
        }
    }

    private fun isPairingReply(bytes: ByteArray): Boolean {
        if (bytes.isEmpty()) return false
        val header = bytes[0].toInt() and 0xFF
        return (bytes.size == PAIR_OK_SIZE && header == PAIR_OK_HEADER) ||
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
        // Sized/filtered as PAIR_OK above; a bad XOR here means corruption.
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
        val psk = try {
            aesGcmDecrypt(kek, nonce12, aad, ct, tag)
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
            // Fresh bond: the counter restarts so the first drive packet is 1.
            lastCounter = 0u,
            alias = ROBOT_HOST,
        )
        return try {
            // Vault first: if the Room write below throws, the key already
            // rests encrypted and the next pairing overwrites it. (Reversing
            // the order would risk the worse outcome — a persisted row whose
            // key is lost.)
            secretStore.savePsk(device.id, psk.toHex())
            deviceRepo.save(device)
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
        2 -> "Pairing failed: code expired"
        3 -> "Pairing failed: robot already bonded"
        else -> "Pairing failed: robot rejected pairing (reason=$reason)"
    }
}
