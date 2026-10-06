package dev.sasnews.amoledwatch.protocol

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** FakeWatch の bulk 特性をそのまま BulkChannel に見せるループバック。 */
private class LoopbackChannel(
    val watch: FakeWatch,
    val mtu: Int = 247,
) : BulkChannel {
    private val reasm = Reassembler()
    private val acks = ArrayDeque<BulkAck>()
    private val ress = ArrayDeque<Res>()
    private var msgSeq = 100

    /** この添字の CHUNK は時計に届かなかったことにする（紛失シミュレート）。 */
    val dropChunkIndexes = mutableSetOf<Int>()
    private var chunkIndex = 0

    private fun pump(type: Int, msgId: Int, payload: ByteArray) {
        for (f in Fragmenter.fragment(type, msgId, payload, mtu)) {
            for (out in watch.writeBulk(f.encode(), mtu)) {
                val c = reasm.feed(FrameCodec.decode(out)) ?: continue
                when (c.type) {
                    Frame.TYPE_BULK_ACK -> acks.add(BulkCodec.decodeAck(c.payload)!!)
                    Frame.TYPE_RES -> ress.add(Res.fromCbor(CborCodec.decode(c.payload)))
                }
            }
        }
    }

    override suspend fun start(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): BulkAck? {
        pump(Frame.TYPE_BULK_START, ++msgSeq, BulkCodec.encodeStart(id, kind, size, sha256, chunk))
        return acks.removeFirstOrNull()
    }

    override suspend fun chunk(id: Int, offset: Long, data: ByteArray) {
        val idx = chunkIndex++
        if (idx in dropChunkIndexes) return
        pump(Frame.TYPE_BULK_CHUNK, ++msgSeq, BulkCodec.encodeChunk(id, offset, data))
    }

    override suspend fun nextAck(timeoutMs: Long): BulkAck? = acks.removeFirstOrNull()

    override suspend fun end(id: Int): Res {
        pump(Frame.TYPE_BULK_END, ++msgSeq, BulkCodec.encodeEnd(id))
        return ress.removeFirstOrNull() ?: Res.Err("internal", "no res")
    }
}

private fun storedZip(entries: List<Pair<String, ByteArray>>): ByteArray {
    val bo = ByteArrayOutputStream()
    ZipOutputStream(bo).use { z ->
        for ((name, data) in entries) {
            val e = ZipEntry(name)
            e.method = ZipEntry.STORED
            e.size = data.size.toLong()
            e.compressedSize = data.size.toLong()
            e.crc = CRC32().also { it.update(data) }.value
            z.putNextEntry(e)
            z.write(data)
            z.closeEntry()
        }
    }
    return bo.toByteArray()
}

private fun themeZip(id: String = "mame", name: String = "まめ", api: Int = 1): ByteArray {
    val manifest = CborCodec.encode(
        Cbor.Cmap(
            linkedMapOf(
                "id" to Cbor.Ctext(id),
                "api" to Cbor.Cint(api.toLong()),
                "name" to Cbor.Ctext(name),
            ),
        ),
    )
    return storedZip(listOf("manifest.cbor" to manifest, "home.bin" to ByteArray(64) { 0x12 }))
}

private fun deflatedZip(): ByteArray {
    val bo = ByteArrayOutputStream()
    ZipOutputStream(bo).use { z ->
        val e = ZipEntry("manifest.cbor") // DEFLATED（既定）
        z.putNextEntry(e)
        z.write(byteArrayOf(1, 2, 3))
        z.closeEntry()
    }
    return bo.toByteArray()
}

class BulkCodecTest {
    @Test
    fun `chunk payload is id u16 LE + offset u32 LE + bytes`() {
        val p = BulkCodec.encodeChunk(0x1234, 0x89ABCDEF, byteArrayOf(9, 8))
        assertEquals(8, p.size)
        assertEquals(0x34, p[0].toInt() and 0xFF)
        assertEquals(0x12, p[1].toInt() and 0xFF)
        assertEquals(0xEF, p[2].toInt() and 0xFF)
        assertEquals(0xCD, p[3].toInt() and 0xFF)
        assertEquals(0xAB, p[4].toInt() and 0xFF)
        assertEquals(0x89, p[5].toInt() and 0xFF)
        assertEquals(9, p[6].toInt() and 0xFF)
        assertEquals(8, p[7].toInt() and 0xFF)
    }

    @Test
    fun `start and end payload are cbor maps`() {
        val start = CborCodec.decode(BulkCodec.encodeStart(7, "theme", 100, ByteArray(32), 220)) as Cbor.Cmap
        assertEquals(7, start.int("id").toInt())
        assertEquals("theme", start.text("kind"))
        assertEquals(100, start.int("size").toInt())
        assertEquals(220, start.int("chunk").toInt())
        val sha = (start.value["sha256"] as? Cbor.Cbytes)?.value
        assertNotNull(sha)
        assertEquals(32, sha!!.size)

        val end = CborCodec.decode(BulkCodec.encodeEnd(7)) as Cbor.Cmap
        assertEquals(7, end.int("id").toInt())
    }

    @Test
    fun `decode ack`() {
        val payload = CborCodec.encode(Cbor.Cmap(mapOf("id" to Cbor.Cint(3), "next" to Cbor.Cint(440))))
        val ack = BulkCodec.decodeAck(payload)
        assertEquals(3, ack!!.id)
        assertEquals(440, ack.next)
        assertNull(BulkCodec.decodeAck(byteArrayOf(0xFF.toByte())))
    }
}

class ThemePackageTest {
    @Test
    fun `inspect valid package`() {
        val info = ThemePackage.inspect(themeZip(id = "mame", name = "まめ"))
        assertEquals("mame", info.id)
        assertEquals("まめ", info.name)
        assertEquals(1, info.api)
    }

    @Test
    fun `name falls back to id`() {
        val manifest = CborCodec.encode(
            Cbor.Cmap(linkedMapOf("id" to Cbor.Ctext("abc"), "api" to Cbor.Cint(1))),
        )
        val info = ThemePackage.inspect(storedZip(listOf("manifest.cbor" to manifest)))
        assertEquals("abc", info.name)
    }

    @Test
    fun `rejects compressed entries`() {
        try {
            ThemePackage.inspect(deflatedZip())
            fail("expected Invalid")
        } catch (e: ThemePackage.Invalid) {
        }
    }

    @Test
    fun `rejects missing manifest`() {
        try {
            ThemePackage.inspect(storedZip(listOf("a.bin" to byteArrayOf(1))))
            fail("expected Invalid")
        } catch (e: ThemePackage.Invalid) {
        }
    }

    @Test
    fun `rejects wrong api and bad id`() {
        try {
            ThemePackage.inspect(themeZip(api = 2))
            fail("expected Invalid")
        } catch (e: ThemePackage.Invalid) {
        }
        try {
            ThemePackage.inspect(themeZip(id = "BAD_ID"))
            fail("expected Invalid")
        } catch (e: ThemePackage.Invalid) {
        }
    }

    @Test
    fun `inspect skin package with png parts`() {
        // v3 skin: 9-slice PNG パーツを含むパッケージも検査を通る。
        val manifest = CborCodec.encode(
            Cbor.Cmap(
                linkedMapOf(
                    "id" to Cbor.Ctext("cyber"),
                    "api" to Cbor.Cint(1),
                    "name" to Cbor.Ctext("サイバー"),
                    "skin" to Cbor.Cmap(
                        linkedMapOf(
                            "button_primary" to Cbor.Cmap(
                                linkedMapOf(
                                    "img" to Cbor.Ctext("button_primary.png"),
                                    "slice" to Cbor.Carray(
                                        listOf(
                                            Cbor.Cint(24), Cbor.Cint(29),
                                            Cbor.Cint(24), Cbor.Cint(29),
                                        ),
                                    ),
                                    "states" to Cbor.Cmap(
                                        mapOf("pressed" to Cbor.Ctext("button_primary_pressed.png")),
                                    ),
                                ),
                            ),
                        ),
                    ),
                ),
            ),
        )
        val info = ThemePackage.inspect(
            storedZip(
                listOf(
                    "manifest.cbor" to manifest,
                    "button_primary.png" to ByteArray(64) { 1 },
                    "button_primary_pressed.png" to ByteArray(64) { 2 },
                ),
            ),
        )
        assertEquals("cyber", info.id)
        assertEquals("サイバー", info.name)
    }

    @Test
    fun `rejects non zip`() {
        try {
            ThemePackage.inspect(byteArrayOf(1, 2, 3, 4))
            fail("expected Invalid")
        } catch (e: ThemePackage.Invalid) {
        }
    }
}

class BulkTransferTest {
    @Test
    fun `normal asset transfer`() = runBlocking {
        val watch = FakeWatch()
        val ch = LoopbackChannel(watch)
        val data = ByteArray(3000) { (it % 251).toByte() }
        var lastSent = 0
        val res = BulkSender().send(ch, "asset", data, transferId = 42) { sent, total ->
            lastSent = sent
            assertEquals(data.size, total)
        }
        assertTrue(res is Res.Ok)
        assertEquals(data.size, lastSent)
        watch.close()
    }

    @Test
    fun `resume with same transfer id`() = runBlocking {
        val watch = FakeWatch()
        val ch = LoopbackChannel(watch)
        val data = ByteArray(2000) { (it % 200).toByte() }
        val sha = java.security.MessageDigest.getInstance("SHA-256").digest(data)
        val id = 77
        val chunk = 220

        // 前半だけ手で送って中断（切断した体）
        val ack0 = ch.start(id, "asset", data.size, sha, chunk)
        assertEquals(0L, ack0!!.next)
        for (off in 0 until 3 * chunk step chunk) {
            ch.chunk(id, off.toLong(), data.copyOfRange(off, off + chunk))
        }

        // 同じ id の BulkSender は ACK.next から再開するはず
        val res = BulkSender().send(ch, "asset", data, transferId = id, chunk = chunk)
        assertTrue(res is Res.Ok)
        watch.close()
    }

    @Test
    fun `lost chunk is recovered via ack`() = runBlocking {
        val watch = FakeWatch()
        val ch = LoopbackChannel(watch)
        ch.dropChunkIndexes.add(3) // 4個目の CHUNK を紛失させる
        val data = ByteArray(1500) { (it % 180).toByte() }
        val res = BulkSender().send(ch, "asset", data, transferId = 5, chunk = 220)
        assertTrue(res is Res.Ok) // sha256 が通る = 抜けた分が再送された
        watch.close()
    }

    @Test
    fun `broken zip gets res err`() = runBlocking {
        val watch = FakeWatch()
        val ch = LoopbackChannel(watch)
        val garbage = ByteArray(500) { (it * 7 % 256).toByte() } // zip ではない
        val res = BulkSender().send(ch, "theme", garbage, transferId = 9)
        assertTrue(res is Res.Err)
        // theme は適用されていない（既定値のまま）
        val settings = settingsGet(watch)
        assertEquals("standard", settings[SettingsKeys.THEME]?.text)
        watch.close()
    }

    @Test
    fun `theme install sets theme setting`() = runBlocking {
        val watch = FakeWatch()
        val ch = LoopbackChannel(watch)
        val zip = themeZip(id = "mame", name = "まめ")
        val res = BulkSender().send(ch, "theme", zip, transferId = 11)
        assertTrue(res is Res.Ok)
        val settings = settingsGet(watch)
        assertEquals("mame", settings[SettingsKeys.THEME]?.text)
        watch.close()
    }

    @Test
    fun `start failure returns err`() = runBlocking {
        val watch = FakeWatch()
        val ch = LoopbackChannel(watch)
        // sha256 が 32B でない START を直接流して拒否させる
        val badStart = CborCodec.encode(
            Cbor.Cmap(mapOf("id" to Cbor.Cint(1), "kind" to Cbor.Ctext("theme"), "size" to Cbor.Cint(10))),
        )
        val outs = Fragmenter.fragment(Frame.TYPE_BULK_START, 55, badStart, 247)
            .flatMap { watch.writeBulk(it.encode()) }
        val resFrame = outs.map { FrameCodec.decode(it) }.first { it.type == Frame.TYPE_RES }
        val res = Res.fromCbor(CborCodec.decode(resFrame.payload))
        assertTrue(res is Res.Err)
        assertEquals("bad_request", (res as Res.Err).code)
        watch.close()
    }

    /** ctrl 経由の settings.get（FakeWatch の REQ 経路）で設定値を読む。 */
    private fun settingsGet(watch: FakeWatch): Map<String, Cbor> {
        val resBytes = watch.write(
            Frame(Frame.TYPE_REQ, 1, CborCodec.encode(Req.SettingsGet(listOf(SettingsKeys.THEME)).toCbor())).encode(),
        )
        val res = Res.fromCbor(CborCodec.decode(FrameCodec.decode(resBytes.single()).payload))
        assertTrue(res is Res.Ok)
        return ((res as Res.Ok).result as? Cbor.Cmap)?.value ?: emptyMap()
    }
}
