package dev.sasnews.amoledwatch.protocol

/**
 * BULK kind=`"dictation"` のリアルタイム音声受信 (docs/protocol-v1.md §BULK kind="dictation")。
 *
 * 時計は音声入力中の IMA-ADPCM 4bit / 16kHz / モノラルの連続ストリームを
 * BULK_START (size=0, sha256=ゼロ32B) → BULK_CHUNK (offset=音声バイト列の累積位置) … →
 * BULK_END で push する。ヘッダは無く、ADPCM のデコーダ状態はチャンクをまたいで連続する。
 *
 * このファイルは「純 Kotlin」で、Android/BLE に依存しない:
 * - [DictationReceiver]: チャンクをインクリメンタルに PCM16LE へデコードし、8チャンクごとに ACK を返す。
 * - [InboundBulkRouter]: 完成済みフレームを dictation / 従来の一括 BULK のどちらへ流すか振り分ける。
 */

/** 時計→スマホの BULK 受信で起きるイベント。 */
sealed interface InboundBulkEvent {
    /** bulk 特性に write-without-response で返す BULK_ACK `{id,next}`。next = 期待する次の offset。 */
    data class Ack(val id: Int, val next: Long) : InboundBulkEvent

    /** 従来の一括転送の完成品 (sha256 検証済み)。 */
    data class Completed(val transfer: IncomingBulk) : InboundBulkEvent

    /** dictation セッション開始 (BLE の接続優先度を上げる契機)。 */
    data class DictationStart(val id: Int) : InboundBulkEvent

    /** デコード済み PCM (signed 16bit LE / モノラル / 16kHz) が届いた。 */
    data class DictationAudio(val id: Int, val pcm: ByteArray) : InboundBulkEvent

    /** dictation セッション終了 (BULK_END または受信側からの打ち切り)。 */
    data class DictationEnd(val id: Int) : InboundBulkEvent
}

/**
 * BULK kind="dictation" の受信機。
 *
 * - ADPCM は [Adpcm.Decoder] をセッションごとに 1 つ保持してデコードする
 *   (チャンク境界で nibble は割れない: 1 バイト = 2 サンプル)。
 * - offset が飛んだら (BLE 取りこぼし) **リアルタイム優先で新しい offset に追従**する。
 *   戻る方向 (再送/重複) は捨てて現在位置を ACK で知らせる。
 * - 8 チャンクごとに ACK を返す ([BulkCodec.ACK_EVERY])。
 */
class DictationReceiver {

    companion object {
        const val KIND = "dictation"

        /** 暴走防止の上限 (8KB/s × 約33分)。超えたらセッションを捨てる。 */
        const val MAX_TOTAL_BYTES = 16L * 1024 * 1024
    }

    /** セッションの統計 (スループットのログ用)。 */
    data class Stats(
        /** 受信した ADPCM バイト数 (欠落分は含まない)。 */
        val adpcmBytes: Long,
        /** 出力した PCM バイト数 (16bit なので adpcmBytes の 4 倍)。 */
        val pcmBytes: Long,
        /** チャンク数。 */
        val chunks: Int,
        /** offset の飛びで捨てた (届かなかった) ADPCM バイト数。 */
        val lostBytes: Long,
    )

    private class Session(val id: Int) {
        var decoder = Adpcm.Decoder()
        var next = 0L
        var sinceAck = 0
        var adpcmBytes = 0L
        var pcmBytes = 0L
        var chunks = 0
        var lostBytes = 0L
    }

    private val sessions = HashMap<Int, Session>()

    /** dictation の BULK_START payload か (kind が "dictation")。 */
    fun isDictationStart(payload: ByteArray): Boolean {
        val m = try {
            CborCodec.decode(payload) as? Cbor.Cmap
        } catch (e: CborCodec.DecodeException) {
            null
        } ?: return false
        return m.text("kind", "") == KIND
    }

    /** セッション開始。不正/重複 id なら false。 */
    fun start(id: Int): Boolean {
        if (id !in 0..Frame.MAX_MSG_ID) return false
        sessions[id] = Session(id)
        return true
    }

    fun isActive(id: Int): Boolean = sessions.containsKey(id)

    fun statsOf(id: Int): Stats? = sessions[id]?.let { s ->
        Stats(s.adpcmBytes, s.pcmBytes, s.chunks, s.lostBytes)
    }

    /** CHUNK を 1 つ処理する。 */
    fun feedChunk(id: Int, offset: Long, data: ByteArray): List<InboundBulkEvent> {
        val s = sessions[id] ?: return emptyList()
        if (data.isEmpty()) return emptyList()

        if (offset != s.next) {
            if (offset < s.next) {
                // 重複/再送。受信済みなので捨てて現在位置を返す (時計が再開できるように)。
                return listOf(InboundBulkEvent.Ack(id, s.next))
            }
            // 前方へ飛んだ = BLE 取りこぼし。時計側は未 ACK 8KB 超過で古い音声を捨てて
            // ADPCM エンコーダを reset しているので、受信側もデコーダを reset してから
            // 新しいチャンクをデコードする (数サンプルのクリックは許容。ASR は頑健)。
            s.lostBytes += offset - s.next
            s.next = offset
            s.decoder = Adpcm.Decoder()
        }
        if (s.adpcmBytes + data.size > MAX_TOTAL_BYTES) {
            sessions.remove(id)
            return listOf(InboundBulkEvent.DictationEnd(id))
        }

        val pcm = Adpcm.pcm16le(s.decoder.decode(data))
        s.next += data.size
        s.adpcmBytes += data.size
        s.pcmBytes += pcm.size
        s.chunks++

        val out = ArrayList<InboundBulkEvent>(2)
        out += InboundBulkEvent.DictationAudio(id, pcm)
        if (++s.sinceAck >= BulkCodec.ACK_EVERY) {
            s.sinceAck = 0
            out += InboundBulkEvent.Ack(id, s.next)
        }
        return out
    }

    /** BULK_END。最終 ACK と終了イベントを返す。 */
    fun end(id: Int): List<InboundBulkEvent> {
        val s = sessions.remove(id) ?: return emptyList()
        return listOf(
            InboundBulkEvent.Ack(id, s.next),
            InboundBulkEvent.DictationEnd(id),
        )
    }

    /** 受信側の都合 (切断・タイムアウト) でセッションを畳む。 */
    fun abort(id: Int): Boolean = sessions.remove(id) != null

    fun abortAll(): List<Int> {
        val ids = sessions.keys.toList()
        sessions.clear()
        return ids
    }
}

/**
 * 完成済み BULK フレームを「dictation のストリーミング」と「従来の一括転送」に振り分ける。
 *
 * - BULK_START の kind が "dictation" なら [DictationReceiver] へ。
 * - それ以外は従来どおり [BulkInbound] へ (size/sha256 必須)。
 * - dictation の CHUNK/END は id で覚えておき、同じ経路へ流す。
 */
class InboundBulkRouter(
    private val bulkInbound: BulkInbound = BulkInbound(),
    private val dictation: DictationReceiver = DictationReceiver(),
) {
    private val dictationIds = HashSet<Int>()

    fun feed(msg: Frame): List<InboundBulkEvent> {
        when (msg.type) {
            Frame.TYPE_BULK_START -> {
                if (dictation.isDictationStart(msg.payload)) {
                    val id = startId(msg.payload) ?: return emptyList()
                    if (!dictation.start(id)) return emptyList()
                    dictationIds += id
                    return listOf(InboundBulkEvent.DictationStart(id))
                }
                return bulkInbound.feed(msg).map { toEvent(it) }
            }

            Frame.TYPE_BULK_CHUNK -> {
                val id = chunkId(msg.payload) ?: return emptyList()
                if (id in dictationIds) {
                    val p = msg.payload
                    val off = (p[2].toLong() and 0xFF) or
                        ((p[3].toLong() and 0xFF) shl 8) or
                        ((p[4].toLong() and 0xFF) shl 16) or
                        ((p[5].toLong() and 0xFF) shl 24)
                    return dictation.feedChunk(id, off, p.copyOfRange(6, p.size))
                }
                return bulkInbound.feed(msg).map { toEvent(it) }
            }

            Frame.TYPE_BULK_END -> {
                val id = endId(msg.payload) ?: return emptyList()
                if (id in dictationIds) {
                    dictationIds -= id
                    return dictation.end(id)
                }
                return bulkInbound.feed(msg).map { toEvent(it) }
            }
        }
        return emptyList()
    }

    /** 切断などで dictation を畳む。 */
    fun abortDictation(): List<Int> {
        val ids = dictationIds.toList()
        dictationIds.clear()
        dictation.abortAll()
        return ids
    }

    private fun toEvent(out: BulkInbound.Out): InboundBulkEvent = when (out) {
        is BulkInbound.Out.Ack -> InboundBulkEvent.Ack(out.id, out.next)
        is BulkInbound.Out.Completed -> InboundBulkEvent.Completed(out.transfer)
    }

    private fun startId(payload: ByteArray): Int? {
        val m = try {
            CborCodec.decode(payload) as? Cbor.Cmap
        } catch (e: CborCodec.DecodeException) {
            null
        } ?: return null
        val id = m.int("id", -1).toInt()
        return id.takeIf { it >= 0 }
    }

    private fun chunkId(payload: ByteArray): Int? {
        if (payload.size < 6) return null
        return (payload[0].toInt() and 0xFF) or ((payload[1].toInt() and 0xFF) shl 8)
    }

    private fun endId(payload: ByteArray): Int? {
        val m = try {
            CborCodec.decode(payload) as? Cbor.Cmap
        } catch (e: CborCodec.DecodeException) {
            null
        } ?: return null
        val id = m.int("id", -1).toInt()
        return id.takeIf { it >= 0 }
    }
}
