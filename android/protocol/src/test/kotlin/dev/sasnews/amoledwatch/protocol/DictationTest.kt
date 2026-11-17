package dev.sasnews.amoledwatch.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * BULK kind="dictation" の受信 (DictationReceiver / InboundBulkRouter) のテスト。
 * docs/protocol-v1.md §BULK kind="dictation" の仕様を固定する。
 */
class DictationTest {

    /** 時計と同じ形式 (IMA-ADPCM 4bit) のトーンを作る。 */
    private fun tonePcm(sec: Int = 1): ShortArray {
        val rate = Adpcm.SAMPLE_RATE
        return ShortArray(rate * sec) { i ->
            (kotlin.math.sin(2.0 * Math.PI * 440.0 * i / rate) * 9000).toInt().toShort()
        }
    }

    private fun startFrame(id: Int): Frame = Frame(
        Frame.TYPE_BULK_START, id,
        BulkCodec.encodeStart(id, DictationReceiver.KIND, 0, ByteArray(32), 232),
    )

    private fun chunkFrame(id: Int, offset: Long, data: ByteArray): Frame =
        Frame(Frame.TYPE_BULK_CHUNK, id, BulkCodec.encodeChunk(id, offset, data))

    private fun endFrame(id: Int): Frame =
        Frame(Frame.TYPE_BULK_END, id, BulkCodec.encodeEnd(id))

    private fun collectPcm(events: List<InboundBulkEvent>): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        for (e in events) if (e is InboundBulkEvent.DictationAudio) out.write(e.pcm)
        return out.toByteArray()
    }

    @Test
    fun incrementalDecodeMatchesOneShotDecode() {
        val pcm = tonePcm(1)
        val adpcm = Adpcm.encodeIma(pcm)
        val router = InboundBulkRouter()
        val out = ArrayList<InboundBulkEvent>()
        out += router.feed(startFrame(7))

        // 3 バイト刻み (nibble/サンプルの区切りと一致しないサイズ) で送る。
        var off = 0
        while (off < adpcm.size) {
            val n = minOf(3, adpcm.size - off)
            out += router.feed(chunkFrame(7, off.toLong(), adpcm.copyOfRange(off, off + n)))
            off += n
        }
        out += router.feed(endFrame(7))

        // チャンクをまたいでデコーダ状態が連続している = 一括デコードと一致する。
        val expected = Adpcm.pcm16le(Adpcm.Decoder().decode(adpcm))
        assertArrayEquals(expected, collectPcm(out))
        assertEquals(1, out.count { it is InboundBulkEvent.DictationStart })
        assertEquals(1, out.count { it is InboundBulkEvent.DictationEnd })
    }

    @Test
    fun acksEveryEightChunks() {
        val router = InboundBulkRouter()
        val acks = ArrayList<Long>()
        router.feed(startFrame(3))
        repeat(20) { i ->
            val n = 232
            val events = router.feed(chunkFrame(3, (i * n).toLong(), ByteArray(n) { 0x11 }))
            events.filterIsInstance<InboundBulkEvent.Ack>().forEach { acks += it.next }
        }
        // 8, 16 チャンク目で ACK。END で最終 ACK (next = 20*232)。
        assertEquals(listOf(8L * 232, 16L * 232), acks)
        val end = router.feed(endFrame(3))
        val finalAck = end.filterIsInstance<InboundBulkEvent.Ack>().single()
        assertEquals(20L * 232, finalAck.next)
        assertEquals(3, finalAck.id)
        assertTrue(end.any { it is InboundBulkEvent.DictationEnd })
    }

    @Test
    fun offsetGapResyncsForRealtime() {
        val rx = DictationReceiver()
        assertTrue(rx.start(5))
        val a = ByteArray(232) { 0x01 }
        val c = ByteArray(232) { 0x03 }
        rx.feedChunk(5, 0, a)
        // b (offset 232) が BLE で落ちた想定 → 464 から届く。リアルタイム優先で追従する。
        val events = rx.feedChunk(5, 464, c)
        val pcm = collectPcm(events)
        // offset が飛んだら時計側エンコーダ reset に合わせてデコーダも reset する
        // → 新しいチャンクは「まっさらなデコーダ」でデコードした PCM と一致する。
        val expected = Adpcm.pcm16le(Adpcm.Decoder().decode(c))
        assertArrayEquals(expected, pcm)
        val stats = rx.statsOf(5)!!
        assertEquals(232L, stats.lostBytes)
        assertEquals(464L, stats.adpcmBytes)  // 実際に受けたチャンクの合計 (欠落は含まない)
        rx.end(5)
    }

    @Test
    fun duplicateChunkIsDroppedAndCurrentOffsetAcked() {
        val router = InboundBulkRouter()
        router.feed(startFrame(9))
        val data = ByteArray(232) { 0x22 }
        router.feed(chunkFrame(9, 0, data))
        val dup = router.feed(chunkFrame(9, 0, data))
        assertEquals(1, dup.size)
        val ack = dup.single() as InboundBulkEvent.Ack
        assertEquals(232L, ack.next)
        assertTrue(dup.none { it is InboundBulkEvent.DictationAudio })
        router.feed(endFrame(9))
    }

    @Test
    fun routerDispatchesBulkAndDictationSeparately() {
        val router = InboundBulkRouter()
        // 従来の一括転送 (size>0, sha 一致) は Completed として流れる。
        val body = ByteArray(16) { it.toByte() }
        val sha = java.security.MessageDigest.getInstance("SHA-256").digest(body)
        router.feed(
            Frame(
                Frame.TYPE_BULK_START, 11,
                BulkCodec.encodeStart(11, "memo", body.size, sha, 220),
            ),
        )
        val done = router.feed(Frame(Frame.TYPE_BULK_CHUNK, 11, BulkCodec.encodeChunk(11, 0, body))) +
            router.feed(Frame(Frame.TYPE_BULK_END, 11, BulkCodec.encodeEnd(11)))
        val completed = done.filterIsInstance<InboundBulkEvent.Completed>().single()
        assertEquals(11, completed.transfer.id)
        assertEquals("memo", completed.transfer.kind)
        assertArrayEquals(body, completed.transfer.bytes)

        // dictation の START は size=0 でも通り、Chaos にならない。
        val start = router.feed(startFrame(12))
        assertTrue(start.single() is InboundBulkEvent.DictationStart)
        assertTrue(router.feed(chunkFrame(12, 0, ByteArray(4))).any {
            it is InboundBulkEvent.DictationAudio
        })
        router.feed(endFrame(12))
    }

    @Test
    fun fakeWatchSimulatesDictation() {
        val watch = FakeWatch()
        val router = InboundBulkRouter()
        val events = ArrayList<InboundBulkEvent>()
        watch.bulkOutListener = { bytes ->
            val f = FrameCodec.decode(bytes) // mtu 4096 で 1 メッセージ = 1 フレーム
            events += router.feed(f)
        }
        val id = watch.simulateDictation(sec = 1, mtuSize = 4096)
        assertTrue(events.any { it is InboundBulkEvent.DictationStart })
        val end = events.filterIsInstance<InboundBulkEvent.DictationEnd>().single()
        assertEquals(id, end.id)
        val pcm = collectPcm(events)
        assertTrue(pcm.isNotEmpty())
        // 8 チャンクごとの ACK (この長さなら必ず 1 回以上)
        assertEquals(id, events.filterIsInstance<InboundBulkEvent.Ack>().first().id)
        watch.close()
    }

    @Test
    fun agentPartialReqOmitsFinalAndTruncatesText() {
        val partial = Req.AgentPartial(42, "あ".repeat(400))
        val p = partial.params()
        assertEquals("agent.partial", partial.method)
        assertEquals(42L, (p.value["id"] as Cbor.Cint).value)
        val text = (p.value["text"] as Cbor.Ctext).value
        assertEquals(Req.AgentPartial.TEXT_MAX_BYTES, text.toByteArray(Charsets.UTF_8).size)
        assertNull(p.value["final"])

        val fin = Req.AgentPartial(42, "確定", final = true).params()
        assertEquals(true, (fin.value["final"] as Cbor.Cbool).value)
        // CBOR に落として戻しても壊れない。
        val decoded = CborCodec.decode(CborCodec.encode(fin)) as Cbor.Cmap
        assertEquals("確定", (decoded.value["text"] as Cbor.Ctext).value)
    }

    @Test
    fun fakeWatchAcceptsAgentPartialAndRejectsTooLong() {
        val watch = FakeWatch()
        val helper = FakeReq()
        val res = helper.request(watch, Req.AgentPartial(5, "こんにちは"))
        assertTrue(res is Res.Ok)
        assertEquals(Triple(5, "こんにちは", false), watch.lastAgentPartial)

        val fin = helper.request(watch, Req.AgentPartial(5, "こんにちは", final = true))
        assertTrue(fin is Res.Ok)
        assertEquals(Triple(5, "こんにちは", true), watch.lastAgentPartial)

        // 768 バイト超は bad_request (Req 側で切るので、生の CBOR で超長を送る)
        val longText = "あ".repeat(300) // 900B
        val raw = Frame(
            Frame.TYPE_REQ, 1,
            CborCodec.encode(
                Cbor.Cmap(
                    mapOf(
                        "m" to Cbor.Ctext("agent.partial"),
                        "p" to Cbor.Cmap(
                            mapOf("id" to Cbor.Cint(5), "text" to Cbor.Ctext(longText)),
                        ),
                    ),
                ),
            ),
        )
        val outs = watch.write(raw.encode(), 247)
        val res2 = Res.fromCbor(
            CborCodec.decode(FrameCodec.decode(outs.first()).payload),
        )
        assertTrue(res2 is Res.Err)
        assertEquals("bad_request", (res2 as Res.Err).code)
        watch.close()
    }

    /** FakeWatch に REQ を送る小さなヘルパ。 */
    private class FakeReq {
        fun request(watch: FakeWatch, req: Req): Res {
            val reasm = Reassembler()
            var res: Res? = null
            for (f in Fragmenter.fragment(Frame.TYPE_REQ, 77, CborCodec.encode(req.toCbor()), 247)) {
                for (out in watch.write(f.encode(), 247)) {
                    val complete = reasm.feed(FrameCodec.decode(out)) ?: continue
                    res = Res.fromCbor(CborCodec.decode(complete.payload))
                }
            }
            return res ?: Res.Err("internal", "no response")
        }
    }
}
