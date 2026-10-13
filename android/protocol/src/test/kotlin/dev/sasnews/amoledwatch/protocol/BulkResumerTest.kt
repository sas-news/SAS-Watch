package dev.sasnews.amoledwatch.protocol

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.security.MessageDigest
import kotlin.math.min

/**
 * T12: BULK レジューム。偽の時計 (受領位置を保持する fake receiver) と
 * 途中で切れる偽チャンネルで、再送時に受領位置から続くことを確認する。
 */
class BulkResumerTest {

    /** 時計側の受信状態 (切断をまたぐ — 実機と同じく消えない)。 */
    private class FakeWatch {
        val received = ByteArray(64 * 1024)
        var receivedLen = 0
        var sha: ByteArray? = null
        var transferId = -1
        var committed = false
        val log = mutableListOf<String>()

        fun start(id: Int, size: Int, sha256: ByteArray): BulkAck {
            // 同じ転送の再送 → 受領位置を返して再開。
            if (transferId == id && sha != null && sha!!.contentEquals(sha256)) {
                log.add("start-resume id=$id next=$receivedLen")
                return BulkAck(id, receivedLen.toLong())
            }
            // 別転送 → 捨てて新規。
            receivedLen = 0
            transferId = id
            sha = sha256.copyOf()
            log.add("start-new id=$id")
            return BulkAck(id, 0)
        }

        fun chunk(id: Int, offset: Long, data: ByteArray) {
            if (id != transferId || offset != receivedLen.toLong()) return
            data.copyInto(received, offset.toInt())
            receivedLen = (offset + data.size).toInt()
        }

        fun end(id: Int): Res {
            if (id != transferId) return Res.Err("internal", "no transfer")
            committed = true
            return Res.Ok(Cbor.Cmap(emptyMap()))
        }
    }

    /** `drops` 個の chunk を書いたら IOException で切れる channel。 */
    private class FlakyChannel(
        private val watch: FakeWatch,
        private val dropAfterBytes: Long,
    ) : BulkChannel {
        var written = 0L

        override suspend fun start(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): BulkAck =
            watch.start(id, size, sha256)

        override suspend fun chunk(id: Int, offset: Long, data: ByteArray) {
            if (written + data.size > dropAfterBytes) throw IOException("disconnected")
            watch.chunk(id, offset, data)
            written += data.size
        }

        override suspend fun nextAck(timeoutMs: Long): BulkAck? = null
        override suspend fun end(id: Int): Res = watch.end(id)
    }

    @Test
    fun resumeContinuesFromWatchOffset() = runBlocking {
        val data = ByteArray(1000) { (it % 251).toByte() }
        val watch = FakeWatch()

        var readyCalls = 0
        var made = 0
        val res = BulkResumer().send(
            // 再接続ごとに新しい channel (= 新しい GATT)。1回目は 500B で切れる。
            newChannel = {
                FlakyChannel(watch, if (made++ == 0) 500 else Long.MAX_VALUE)
            },
            awaitReady = { readyCalls++; true },
            kind = "theme", data = data,
            transferId = BulkResumer.transferIdOf(data),
            chunk = 100,
        )
        assertTrue("expected Ok, got $res", res is Res.Ok)
        assertEquals(1, readyCalls)  // 1 回切断 → 1 回再接続待ち
        // 時計側は 1 回目で 500B 受け取り、2 回目の start で next=500 を返す。
        assertTrue(watch.log.any { it.contains("start-resume") && it.contains("next=500") })
        assertEquals(1000, watch.receivedLen)
        assertArrayEquals(data, watch.received.copyOf(1000))
        assertTrue(watch.committed)
    }

    @Test
    fun resumeAcrossMultipleDrops() = runBlocking {
        val data = ByteArray(1000) { (it % 97).toByte() }
        val watch = FakeWatch()
        // 300B, あと 400B で切れる (300+400=700) → 残り 300B は 3 回目で。
        val limits = listOf(300L, 400L, Long.MAX_VALUE)
        var call = 0
        val res = BulkResumer().send(
            newChannel = { FlakyChannel(watch, limits.getOrElse(call++) { Long.MAX_VALUE }) },
            awaitReady = { true },
            kind = "theme", data = data,
            transferId = BulkResumer.transferIdOf(data),
            chunk = 100,
        )
        assertTrue("expected Ok, got $res", res is Res.Ok)
        assertTrue(watch.log.any { it.contains("next=300") })
        assertTrue(watch.log.any { it.contains("next=700") })
        assertArrayEquals(data, watch.received.copyOf(1000))
    }

    @Test
    fun givesUpAfterMaxAttempts() = runBlocking {
        val data = ByteArray(500) { 1 }
        val watch = FakeWatch()
        var attempts = 0
        val res = BulkResumer(maxAttempts = 2).send(
            newChannel = { FlakyChannel(watch, 0) },
            awaitReady = { true },
            kind = "theme", data = data,
            transferId = 7, chunk = 100,
        )
        assertTrue("expected Err, got $res", res is Res.Err)
        assertEquals(0, watch.receivedLen)
        assertTrue(!watch.committed)
    }

    @Test
    fun explicitRejectionDoesNotRetry() = runBlocking {
        val data = ByteArray(500) { 1 }
        val watch = FakeWatch()
        val badStart = object : BulkChannel {
            override suspend fun start(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): BulkAck? =
                null  // 拒否
            override suspend fun chunk(id: Int, offset: Long, data: ByteArray) = Unit
            override suspend fun nextAck(timeoutMs: Long): BulkAck? = null
            override suspend fun end(id: Int): Res = Res.Err("bad_request", "no")
        }
        var readyCalls = 0
        val res = BulkResumer().send(
            newChannel = { badStart },
            awaitReady = { readyCalls++; true },
            kind = "theme", data = data, transferId = 7,
        )
        // start=null → "internal" Err → これはリトライ対象 (切断と見分けがつかない)。
        assertTrue(res is Res.Err)
        assertEquals(BulkResumer().let { 3 - 1 }, readyCalls)  // maxAttempts-1 回待つ
    }
}
