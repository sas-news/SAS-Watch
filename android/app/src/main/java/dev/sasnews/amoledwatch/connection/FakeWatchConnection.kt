package dev.sasnews.amoledwatch.connection

import dev.sasnews.amoledwatch.protocol.BulkAck
import dev.sasnews.amoledwatch.protocol.BulkChannel
import dev.sasnews.amoledwatch.protocol.BulkCodec
import dev.sasnews.amoledwatch.protocol.BulkInbound
import dev.sasnews.amoledwatch.protocol.CborCodec
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.FakeWatch
import dev.sasnews.amoledwatch.protocol.Frame
import dev.sasnews.amoledwatch.protocol.FrameCodec
import dev.sasnews.amoledwatch.protocol.FrameException
import dev.sasnews.amoledwatch.protocol.Fragmenter
import dev.sasnews.amoledwatch.protocol.IncomingBulk
import dev.sasnews.amoledwatch.protocol.Reassembler
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.protocol.Res
import dev.sasnews.amoledwatch.protocol.toCbor
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.Dispatchers

/**
 * FakeWatch（protocol モジュール）を通信路に見せるデバッグ用リンク。
 * Frame/CBOR/フラグメントの経路は実機と同じにしてある。
 */
class FakeWatchConnection(private val scope: CoroutineScope) : WatchLink {

    /** デモ操作用に中身を公開（電池イベントを発生させる等）。 */
    val fake = FakeWatch()

    private val _state = MutableStateFlow<LinkState>(LinkState.Connecting(1))
    override val state: StateFlow<LinkState> = _state

    private val _events = MutableSharedFlow<Evt>(extraBufferCapacity = 64)
    override val events: SharedFlow<Evt> = _events

    private val resReassembler = Reassembler()
    private val msgIds = AtomicInteger(1)
    @Volatile private var closed = false

    override val bulk: BulkChannel = FakeBulkChannel()

    /** 時計→スマホの BULK 転送の完成品 (kind="memo"/"agent" 等)。 */
    private val _incomingBulk = MutableSharedFlow<IncomingBulk>(extraBufferCapacity = 8)
    override val incomingBulk: SharedFlow<IncomingBulk> = _incomingBulk
    private val inboundReassembler = Reassembler()
    private val bulkInbound = BulkInbound()

    init {
        fake.evtListener = { bytes ->
            val evt = runCatching {
                Evt.fromCbor(CborCodec.decode(FrameCodec.decode(bytes).payload))
            }.getOrNull()
            if (evt != null) _events.tryEmit(evt)
        }
        fake.bulkOutListener = { bytes ->
            try {
                inboundReassembler.feed(FrameCodec.decode(bytes))?.let { complete ->
                    // FakeWatch の送信側は ACK を待たないので Completed だけ拾う
                    bulkInbound.feed(complete).forEach {
                        if (it is BulkInbound.Out.Completed) {
                            _incomingBulk.tryEmit(it.transfer)
                        }
                    }
                }
            } catch (_: FrameException) {
            }
        }
        scope.launch {
            delay(500) // 接続待ちっぽさを演出
            if (!closed) _state.value = LinkState.Connected
        }
    }

    private fun nextMsgId(): Int = msgIds.getAndIncrement() and Frame.MAX_MSG_ID

    override suspend fun request(req: Req): Res = withContext(Dispatchers.Default) {
        if (closed) return@withContext Res.Err("internal", "closed")
        val id = nextMsgId()
        try {
            val frames = Fragmenter.fragment(Frame.TYPE_REQ, id, CborCodec.encode(req.toCbor()), 247)
            var res: Res? = null
            for (f in frames) {
                for (resBytes in fake.write(f.encode(), 247)) {
                    val frag = FrameCodec.decode(resBytes)
                    val complete = resReassembler.feed(frag) ?: continue
                    res = Res.fromCbor(CborCodec.decode(complete.payload))
                }
            }
            res ?: Res.Err("internal", "no response")
        } catch (e: Exception) {
            Res.Err("internal", e.message ?: "request failed")
        }
    }

    /** FakeWatch は BULK フレームを経由せず実体をそのまま返す。 */
    override suspend fun fetchBulk(id: Int, sha256: ByteArray, timeoutMs: Long): ByteArray? {
        val blob = fake.audioBlobFor(id) ?: return null
        val sha = java.security.MessageDigest.getInstance("SHA-256").digest(blob)
        return if (sha.contentEquals(sha256)) blob else null
    }

    /**
     * FakeWatch の bulk 特性を BulkChannel に見せる。
     * `fake.writeBulk` は同期的に ACK/RES のフレーム列を返すので、
     * それを再構成して ACK キュー / RES キューに振り分ける。
     */
    inner class FakeBulkChannel : BulkChannel {
        private val bulkReassembler = Reassembler()
        private val acks = ArrayDeque<BulkAck>()
        private val ress = ArrayDeque<Res>()

        private fun pump(type: Int, msgId: Int, payload: ByteArray) {
            for (f in Fragmenter.fragment(type, msgId, payload, 247)) {
                for (out in fake.writeBulk(f.encode(), 247)) {
                    val c = bulkReassembler.feed(FrameCodec.decode(out)) ?: continue
                    when (c.type) {
                        Frame.TYPE_BULK_ACK -> BulkCodec.decodeAck(c.payload)?.let { acks.add(it) }
                        Frame.TYPE_RES -> ress.add(Res.fromCbor(CborCodec.decode(c.payload)))
                    }
                }
            }
        }

        override suspend fun start(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): BulkAck? {
            if (closed) return null
            pump(Frame.TYPE_BULK_START, nextMsgId(), BulkCodec.encodeStart(id, kind, size, sha256, chunk))
            // START を拒否した場合は RES err が返る（ACK は来ない）
            return acks.removeFirstOrNull()
        }

        override suspend fun chunk(id: Int, offset: Long, data: ByteArray) {
            if (closed) throw java.io.IOException("closed")
            pump(Frame.TYPE_BULK_CHUNK, id, BulkCodec.encodeChunk(id, offset, data))
        }

        override suspend fun nextAck(timeoutMs: Long): BulkAck? = acks.removeFirstOrNull()

        override suspend fun end(id: Int): Res {
            if (closed) return Res.Err("internal", "closed")
            pump(Frame.TYPE_BULK_END, nextMsgId(), BulkCodec.encodeEnd(id))
            return ress.removeFirstOrNull() ?: Res.Err("internal", "no res")
        }
    }

    override fun close() {
        closed = true
        fake.close()
        _state.value = LinkState.Disconnected
    }
}
