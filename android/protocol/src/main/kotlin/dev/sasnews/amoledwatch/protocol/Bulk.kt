package dev.sasnews.amoledwatch.protocol

import java.security.MessageDigest

/**
 * BULK 転送の送信側（docs/protocol-v1.md §BULK）。
 *
 * - START payload: CBOR `{id, kind, size, sha256:<bytes32>, chunk}`
 * - CHUNK payload: `transfer_id:u16 | offset:u32 | bytes...`（u16/u32 は little endian）
 * - ACK payload: CBOR `{id, next}`（8 チャンクごと / 再開時 / オフセット不整合時）
 * - END payload: CBOR `{id}` → 時計は sha256 を検証して RES を返す
 */

/** BULK_ACK `{id, next}`。next = 時計が次に受け取るべきオフセット。 */
data class BulkAck(val id: Int, val next: Long)

/** BULK 転送路（bulk 特性 write + notify、END/START 失敗時の RES は ctrl notify）。 */
interface BulkChannel {
    /**
     * BULK_START を送る。戻り値は時計が返す ACK（`next` = 再開オフセット）。
     * 時計が拒否した（ctrl に RES err）場合やタイムアウトは null。
     */
    suspend fun start(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): BulkAck?

    /** BULK_CHUNK を 1 つ送る。data はそのまま payload 末尾に乗るバイト列。 */
    suspend fun chunk(id: Int, offset: Long, data: ByteArray)

    /** 途中の BULK_ACK を読む。timeoutMs 以内に来なければ null（0 = 非ブロックのポーリング）。 */
    suspend fun nextAck(timeoutMs: Long): BulkAck?

    /** BULK_END を送り、ctrl に返る RES（ok / err）を返す。 */
    suspend fun end(id: Int): Res
}

object BulkCodec {
    /** kind の最大長（core `kBulkKindMax` = 15。"firmware" まで入る）。 */
    const val MAX_KIND_LEN = 15
    const val DEFAULT_CHUNK = 220
    const val ACK_EVERY = 8

    fun encodeStart(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): ByteArray {
        require(id in 0..Frame.MAX_MSG_ID) { "id out of range: $id" }
        require(sha256.size == 32) { "sha256 must be 32 bytes" }
        require(kind.isNotEmpty() && kind.toByteArray(Charsets.UTF_8).size <= MAX_KIND_LEN) {
            "kind too long or empty: $kind"
        }
        return CborCodec.encode(
            Cbor.Cmap(
                mapOf(
                    "id" to Cbor.Cint(id.toLong()),
                    "kind" to Cbor.Ctext(kind),
                    "size" to Cbor.Cint(size.toLong()),
                    "sha256" to Cbor.Cbytes(sha256),
                    "chunk" to Cbor.Cint(chunk.toLong()),
                ),
            ),
        )
    }

    fun encodeEnd(id: Int): ByteArray =
        CborCodec.encode(Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong()))))

    /** CHUNK payload: `id:u16 LE | offset:u32 LE | bytes`。 */
    fun encodeChunk(id: Int, offset: Long, data: ByteArray): ByteArray {
        require(id in 0..Frame.MAX_MSG_ID) { "id out of range: $id" }
        require(offset in 0..0xFFFFFFFFL) { "offset out of range: $offset" }
        val out = ByteArray(6 + data.size)
        out[0] = (id and 0xFF).toByte()
        out[1] = ((id ushr 8) and 0xFF).toByte()
        out[2] = (offset and 0xFF).toByte()
        out[3] = ((offset ushr 8) and 0xFF).toByte()
        out[4] = ((offset ushr 16) and 0xFF).toByte()
        out[5] = ((offset ushr 24) and 0xFF).toByte()
        data.copyInto(out, 6)
        return out
    }

    /** ACK payload `{id, next}` を読む。形が違えば null。 */
    fun decodeAck(payload: ByteArray): BulkAck? {
        val m = try {
            CborCodec.decode(payload) as? Cbor.Cmap
        } catch (e: CborCodec.DecodeException) {
            null
        } ?: return null
        return BulkAck(m.int("id", -1).toInt(), m.int("next", -1))
    }
}

/**
 * BULK 送信の状態機械。
 * start → 返ってきた ACK の next（再開位置）から chunk バイトずつ送り、
 * END で RES を取る。途中の ACK は進捗と同期ずれの検知にだけ使い、待ちすぎない。
 */
class BulkSender {

    suspend fun send(
        ch: BulkChannel,
        kind: String,
        data: ByteArray,
        transferId: Int,
        chunk: Int = BulkCodec.DEFAULT_CHUNK,
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): Res {
        require(chunk > 0) { "chunk must be > 0" }
        val sha256 = MessageDigest.getInstance("SHA-256").digest(data)
        val ack = ch.start(transferId, kind, data.size, sha256, chunk)
            ?: return Res.Err("internal", "BULK_START が受理されませんでした")

        var offset = ack.next
        if (offset < 0 || offset > data.size) offset = 0
        onProgress(offset.toInt(), data.size)

        while (offset < data.size) {
            val n = minOf(chunk.toLong(), data.size - offset).toInt()
            ch.chunk(transferId, offset, data.copyOfRange(offset.toInt(), offset.toInt() + n))
            offset += n
            onProgress(offset.toInt(), data.size)

            // 届いていれば途中 ACK を全部拾う（待たない）。時計の受信位置が
            // こちらより手前なら、抜けたチャンクから送り直す。
            // 滞留した古い ACK で何度も巻き戻らないよう、最新（最大）の受信位置を使う。
            var receiver: Long? = null
            while (true) {
                val a = ch.nextAck(0) ?: break
                if (a.id == transferId && a.next in 0..data.size.toLong()) {
                    receiver = maxOf(receiver ?: a.next, a.next)
                }
            }
            if (receiver != null && receiver < offset) {
                offset = receiver
            }
        }
        return ch.end(transferId)
    }
}
