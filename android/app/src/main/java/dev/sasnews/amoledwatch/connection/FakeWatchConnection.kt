package dev.sasnews.amoledwatch.connection

import dev.sasnews.amoledwatch.protocol.CborCodec
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.FakeWatch
import dev.sasnews.amoledwatch.protocol.Frame
import dev.sasnews.amoledwatch.protocol.FrameCodec
import dev.sasnews.amoledwatch.protocol.Fragmenter
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

    init {
        fake.evtListener = { bytes ->
            val evt = runCatching {
                Evt.fromCbor(CborCodec.decode(FrameCodec.decode(bytes).payload))
            }.getOrNull()
            if (evt != null) _events.tryEmit(evt)
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

    override fun close() {
        closed = true
        fake.close()
        _state.value = LinkState.Disconnected
    }
}
