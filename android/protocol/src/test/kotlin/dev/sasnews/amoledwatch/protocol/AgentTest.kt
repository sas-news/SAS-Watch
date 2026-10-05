package dev.sasnews.amoledwatch.protocol

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * FakeWatch → BulkInbound → agent.reply の流れを試す。
 * アプリ側の onEvent / incomingBulk と同じ経路をそのまま再現している。
 */
class AgentTest {

    private fun collectIncoming(watch: FakeWatch): Pair<MutableList<IncomingBulk>, MutableList<BulkAck>> {
        val inbox = mutableListOf<IncomingBulk>()
        val acks = mutableListOf<BulkAck>()
        val reasm = Reassembler()
        val inbound = BulkInbound()
        watch.bulkOutListener = { bytes ->
            inboundReasm(inbox, acks, reasm, inbound, bytes)
        }
        return inbox to acks
    }

    private fun inboundReasm(
        inbox: MutableList<IncomingBulk>,
        acks: MutableList<BulkAck>,
        reasm: Reassembler,
        inbound: BulkInbound,
        bytes: ByteArray,
    ) {
        val complete = reasm.feed(FrameCodec.decode(bytes)) ?: return
        for (out in inbound.feed(complete)) {
            when (out) {
                is BulkInbound.Out.Ack ->
                    acks.add(BulkAck(out.id, out.next))
                is BulkInbound.Out.Completed -> inbox.add(out.transfer)
            }
        }
    }

    @Test
    fun pushedAgentAudioArrivesAsIncomingBulk() {
        val watch = FakeWatch()
        val (inbox, acks) = collectIncoming(watch)

        val id = watch.simulateAgentAudio(sec = 2)
        assertEquals(1, inbox.size)
        val t = inbox[0]
        assertEquals(id, t.id)
        assertEquals("agent", t.kind)
        // ADP1 ヘッダが付いた音声データが届く
        assertEquals('A'.code.toByte(), t.bytes[0])
        assertNotNull(Adpcm.adp1ToWav(t.bytes))
        // 8チャンクごと + 最終ACK が返る
        assertEquals(t.id, acks.last().id)
        assertEquals(t.bytes.size.toLong(), acks.last().next)
        watch.close()
    }

    @Test
    fun agentRequestEvtAndReplyRoundTrip() = runBlocking {
        val watch = FakeWatch()
        var got: Evt? = null
        watch.evtListener = { bytes ->
            got = Evt.fromCbor(CborCodec.decode(FrameCodec.decode(bytes).payload))
        }
        val id = watch.simulateAgentRequest("今日の予定は？")
        assertEquals(Evt.AgentRequest(id, "今日の予定は？"), got)

        // アプリ → 時計: agent.reply REQ が通り lastAgentReply に残る
        val req = Req.AgentReply(id, "15時に会議があります")
        val frames = Fragmenter.fragment(
            Frame.TYPE_REQ, 42,
            CborCodec.encode(req.toCbor()), 247,
        )
        val reasm = Reassembler()
        var res: Res? = null
        for (f in frames) {
            for (out in watch.write(f.encode(), 247)) {
                res = Res.fromCbor(
                    CborCodec.decode(reasm.feed(FrameCodec.decode(out))!!.payload),
                )
            }
        }
        assertTrue(res is Res.Ok)
        assertEquals(id to "15時に会議があります", watch.lastAgentReply)
        watch.close()
    }

    @Test
    fun droppedChunkTriggersResendAck() {
        val watch = FakeWatch()
        val reasm = Reassembler()
        val inbound = BulkInbound()
        val inbox = mutableListOf<IncomingBulk>()
        val acks = mutableListOf<BulkAck>()
        watch.bulkOutListener = { bytes ->
            inboundReasm(inbox, acks, reasm, inbound, bytes)
        }

        // 途中の CHUNK メッセージを1つ丸ごと落とす → 受領位置を返す ACK が出て、
        // 完成しない。(mtu を大きくして1メッセージ=1フレームにし、欠落を再現)
        var dropped = false
        val orig = watch.bulkOutListener
        watch.bulkOutListener = listener@{ bytes ->
            val f = FrameCodec.decode(bytes)
            if (!dropped && f.type == Frame.TYPE_BULK_CHUNK) {
                dropped = true
                return@listener  // 欠落
            }
            orig!!(bytes)
        }
        val id = watch.simulateAgentAudio(sec = 2, mtuSize = 4096)
        assertEquals(0, inbox.size)
        // ずれ検出の ACK{next=0} が出ている
        assertTrue(acks.any { it.id == id && it.next == 0L })
        watch.close()
    }
}
