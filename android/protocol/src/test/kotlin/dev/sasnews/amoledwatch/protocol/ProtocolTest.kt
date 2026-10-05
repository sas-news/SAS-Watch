package dev.sasnews.amoledwatch.protocol

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class Crc16Test {
    @Test
    fun `crc16 of 123456789 is 0x29B1`() {
        assertEquals(0x29B1, Crc16.compute("123456789".toByteArray()))
    }

    @Test
    fun `crc16 of empty is initial value`() {
        assertEquals(0xFFFF, Crc16.compute(byteArrayOf()))
    }

    @Test
    fun `crc16 differs on flipped bit`() {
        val a = "123456789".toByteArray()
        val b = a.copyOf().also { it[4] = (it[4].toInt() xor 1).toByte() }
        assertTrue(Crc16.compute(a) != Crc16.compute(b))
    }
}

class FrameTest {
    @Test
    fun `encode decode roundtrip`() {
        val f = Frame(Frame.TYPE_REQ, 0x1234, byteArrayOf(1, 2, 3, 0xFF.toByte()), flags = 0, seq = 0)
        val bytes = f.encode()
        assertEquals(Frame.VERSION.toLong(), bytes[0].toLong() and 0xFF)
        assertEquals(Frame.TYPE_REQ.toLong(), bytes[1].toLong() and 0xFF)
        assertEquals(4, (bytes[6].toInt() and 0xFF) or ((bytes[7].toInt() and 0xFF) shl 8))
        assertEquals(0x34, bytes[4].toInt() and 0xFF)
        assertEquals(0x12, bytes[5].toInt() and 0xFF)
        assertEquals(f, FrameCodec.decode(bytes))
    }

    @Test
    fun `decode rejects broken crc`() {
        val bytes = Frame(Frame.TYPE_RES, 7, byteArrayOf(10, 20)).encode()
        bytes[bytes.size - 1] = (bytes[bytes.size - 1].toInt() xor 0xFF).toByte()
        try {
            FrameCodec.decode(bytes)
            org.junit.Assert.fail("expected FrameException")
        } catch (e: FrameException) {
            assertTrue(e.message!!.contains("crc"))
        }
    }

    @Test
    fun `decode rejects bad version`() {
        val bytes = Frame(Frame.TYPE_REQ, 1, byteArrayOf(0)).encode()
        bytes[0] = 2
        // CRC も再計算して「バージョンだけ違う」フレームにする
        val len = bytes.size - Frame.HEADER_SIZE - Frame.CRC_SIZE
        val crc = Crc16.compute(bytes, 0, Frame.HEADER_SIZE + len)
        bytes[bytes.size - 2] = (crc and 0xFF).toByte()
        bytes[bytes.size - 1] = ((crc ushr 8) and 0xFF).toByte()
        try {
            FrameCodec.decode(bytes)
            org.junit.Assert.fail("expected FrameException")
        } catch (e: FrameException) {
            assertTrue(e.message!!.contains("version"))
        }
    }

    @Test
    fun `decode rejects short frame`() {
        try {
            FrameCodec.decode(byteArrayOf(1, 2, 3))
            org.junit.Assert.fail("expected FrameException")
        } catch (e: FrameException) {
        }
    }
}

class CborTest {
    @Test
    fun `roundtrip all types`() {
        val v = Cbor.Cmap(
            mapOf(
                "int" to Cbor.Cint(42),
                "neg" to Cbor.Cint(-1000000),
                "big" to Cbor.Cint(4_000_000_000),
                "text" to Cbor.Ctext("こんにちは"),
                "bytes" to Cbor.Cbytes(byteArrayOf(1, 2, 0xFE.toByte())),
                "arr" to Cbor.Carray(listOf(Cbor.Cint(1), Cbor.Cbool(true), Cbor.Cnull)),
                "nested" to Cbor.Cmap(mapOf("a" to Cbor.Cbool(false))),
            ),
        )
        assertEquals(v, CborCodec.decode(CborCodec.encode(v)))
    }

    @Test
    fun `int boundary encodings`() {
        for (n in listOf(0L, 23L, 24L, 255L, 256L, 65535L, 65536L, -1L, -24L, -25L, -65537L, Long.MIN_VALUE + 1)) {
            val v = Cbor.Cint(n)
            assertEquals(v, CborCodec.decode(CborCodec.encode(v)))
        }
    }

    @Test
    fun `decode rejects trailing bytes`() {
        val b = CborCodec.encode(Cbor.Cint(1)) + byteArrayOf(9, 9)
        try {
            CborCodec.decode(b)
            org.junit.Assert.fail("expected DecodeException")
        } catch (e: CborCodec.DecodeException) {
        }
    }
}

class FragmentTest {
    private fun big(n: Int) = ByteArray(n) { (it % 251).toByte() }

    @Test
    fun `small payload is one frame`() {
        val frames = Fragmenter.fragment(Frame.TYPE_REQ, 1, big(10), 247)
        assertEquals(1, frames.size)
        assertFalse(frames[0].hasMore)
    }

    @Test
    fun `large payload splits and reassembles`() {
        val payload = big(2000)
        val frames = Fragmenter.fragment(Frame.TYPE_REQ, 9, payload, 247)
        assertTrue(frames.size > 1)
        frames.forEachIndexed { i, f ->
            assertEquals(i, f.seq)
            assertEquals(i < frames.size - 1, f.hasMore)
            assertTrue(f.encode().size <= 247 - 3)
        }
        val re = Reassembler()
        var done: Frame? = null
        for (f in frames) done = re.feed(f) ?: done
        assertArrayEquals(payload, done!!.payload)
    }

    @Test
    fun `out of order fragment throws`() {
        val frames = Fragmenter.fragment(Frame.TYPE_REQ, 9, big(2000), 247)
        val re = Reassembler()
        re.feed(frames[0])
        try {
            re.feed(frames[2]) // seq=1 を飛ばす
            org.junit.Assert.fail("expected FrameException")
        } catch (e: FrameException) {
            assertTrue(e.message!!.contains("seq"))
        }
    }

    @Test
    fun `encoded fragments decode and reassemble`() {
        val payload = big(1500)
        val wire = Fragmenter.fragment(Frame.TYPE_RES, 42, payload, 128).map { it.encode() }
        val re = Reassembler()
        var done: Frame? = null
        for (b in wire) done = re.feed(FrameCodec.decode(b)) ?: done
        assertEquals(42, done!!.msgId)
        assertArrayEquals(payload, done.payload)
    }
}

class FakeWatchTest {
    @Test
    fun `hello and settings roundtrip`() {
        val watch = FakeWatch()
        val resBytes = watch.write(Frame(Frame.TYPE_REQ, 1, CborCodec.encode(Req.Hello(app = "0.1.0").toCbor())).encode())
        val res = Res.fromCbor(CborCodec.decode(FrameCodec.decode(resBytes.single()).payload))
        assertTrue(res is Res.Ok)
        val hello = HelloResult.fromCbor((res as Res.Ok).result)
        assertNotNull(hello)
        assertTrue(hello!!.caps.contains("timer"))
        watch.close()
    }

    @Test
    fun `timer fires finished event`() {
        val watch = FakeWatch()
        val events = ArrayList<Evt>()
        watch.evtListener = { events.add(Evt.fromCbor(CborCodec.decode(FrameCodec.decode(it).payload))!!) }
        watch.write(Frame(Frame.TYPE_REQ, 2, CborCodec.encode(Req.TimerStart(1).toCbor())).encode())
        Thread.sleep(1400)
        assertTrue(events.any { it is Evt.TimerFinished })
        watch.close()
    }

    @Test
    fun `memo list voice memo and delete`() {
        val watch = FakeWatch()
        val events = ArrayList<Evt>()
        watch.evtListener = { events.add(Evt.fromCbor(CborCodec.decode(FrameCodec.decode(it).payload))!!) }

        watch.write(
            Frame(Frame.TYPE_REQ, 10, CborCodec.encode(Req.MemoCreate("牛乳を買う").toCbor())).encode(),
        )
        watch.simulateVoiceMemo(2)

        val listBytes = watch.write(
            Frame(Frame.TYPE_REQ, 11, CborCodec.encode(Req.MemoList().toCbor())).encode(),
        )
        val listRes = Res.fromCbor(CborCodec.decode(FrameCodec.decode(listBytes.single()).payload))
        assertTrue(listRes is Res.Ok)
        val list = MemoListResult.fromCbor((listRes as Res.Ok).result)
        assertNotNull(list)
        assertEquals(2, list!!.memos.size)
        // 新しい順: 音声メモが先
        assertEquals("voice", list.memos[0].kind)
        assertEquals(2, list.memos[0].sec)
        assertEquals("text", list.memos[1].kind)

        val voiceId = list.memos[0].id
        val audioBytes = watch.write(
            Frame(Frame.TYPE_REQ, 12, CborCodec.encode(Req.MemoAudioGet(voiceId).toCbor())).encode(),
        )
        val audioRes = Res.fromCbor(CborCodec.decode(FrameCodec.decode(audioBytes.single()).payload))
        assertTrue(audioRes is Res.Ok)
        val info = MemoAudioInfo.fromCbor((audioRes as Res.Ok).result)
        assertNotNull(info)
        assertEquals(32, info!!.sha256.size)
        assertTrue(info.size > 16)
        // デコーダが実体を受け取れるか
        assertNotNull(Adpcm.decodePcm(watch.audioBlobFor(voiceId)!!))

        val delBytes = watch.write(
            Frame(Frame.TYPE_REQ, 13, CborCodec.encode(Req.MemoDelete(voiceId).toCbor())).encode(),
        )
        assertTrue(
            Res.fromCbor(CborCodec.decode(FrameCodec.decode(delBytes.single()).payload))
                is Res.Ok,
        )
        // 消した後の audio.get は not_found
        val nfBytes = watch.write(
            Frame(Frame.TYPE_REQ, 14, CborCodec.encode(Req.MemoAudioGet(voiceId).toCbor())).encode(),
        )
        val nf = Res.fromCbor(CborCodec.decode(FrameCodec.decode(nfBytes.single()).payload))
        assertTrue(nf is Res.Err)
        assertEquals("not_found", (nf as Res.Err).code)

        assertTrue(events.any { it is Evt.MemoSaved && it.kind == "voice" })
        watch.close()
    }

    @Test
    fun `unsupported proto returns error`() {
        val watch = FakeWatch()
        val resBytes = watch.write(
            Frame(Frame.TYPE_REQ, 3, CborCodec.encode(Req.Hello(proto = 99, app = "x").toCbor())).encode(),
        )
        val res = Res.fromCbor(CborCodec.decode(FrameCodec.decode(resBytes.single()).payload))
        assertTrue(res is Res.Err)
        assertEquals("unsupported_proto", (res as Res.Err).code)
        watch.close()
    }
}
