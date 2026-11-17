package dev.sasnews.amoledwatch.agent

import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** dictation の 1 回分パイプライン (部分表示 → 確定 → 返答) のテスト。 */
class DictationSessionTest {

    /** テスト用の realtime STT。feed で部分結果を出し、finish で確定する。 */
    private class FakeSttEngine(
        private val onPartial: (String) -> Unit,
        private val onError: (String) -> Unit,
        private val finalText: String? = "確定テキスト",
    ) : SttEngine {
        override var partialText: String = "途中"
            private set
        override var errorText: String? = null
            private set
        override val droppedBytes: Long = 0
        var started = false
            private set
        var fedBytes = 0L
            private set
        var finished = false
            private set
        var cancelled = false
            private set

        override fun start() {
            started = true
        }

        override fun feed(pcm: ByteArray) {
            fedBytes += pcm.size
            partialText = "途中(部分)"
            onPartial(partialText)
        }

        override suspend fun finish(timeoutMs: Long): String? {
            finished = true
            return finalText
        }

        override fun cancel() {
            cancelled = true
        }

        fun fail(message: String) {
            errorText = message
            onError(message)
        }
    }

    @Test
    fun realtimePathSendsPartialsFinalThenReply() = runBlocking {
        var fake: FakeSttEngine? = null
        val partials = mutableListOf<Pair<String, Boolean>>()
        val replies = mutableListOf<String>()
        val asked = mutableListOf<String>()
        var finishedCalls = 0
        val session = DictationSession(
            id = 1,
            createStt = { onPartial, onError ->
                FakeSttEngine(onPartial, onError).also { fake = it }
            },
            ask = { q -> asked += q; "返答です" },
            onPartial = { t, f -> partials += t to f },
            onReply = { replies += it },
            scope = this,
            onFinished = { finishedCalls++ },
        )
        session.onPcm(ByteArray(640))
        session.onPcm(ByteArray(640))
        delay(1) // 部分結果は scope.launch で届く
        assertEquals(2, partials.size)  // チャンクごとに 1 回
        assertEquals("途中(部分)", partials[0].first)
        assertFalse(partials[0].second)
        assertEquals(1280L, fake!!.fedBytes)

        session.finish()
        assertEquals("確定テキスト", partials.last().first)
        assertTrue(partials.last().second)
        assertEquals(listOf("確定テキスト"), asked)
        assertEquals(listOf("返答です"), replies)
        assertTrue(fake!!.finished)
        assertEquals(1280L, session.stats().pcmBytes)
        assertEquals(1, finishedCalls)  // BLE の接続優先度を戻す契機
    }

    @Test
    fun silenceTimeoutFinalizesOnce() = runBlocking {
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 2,
            createStt = { onPartial, onError -> FakeSttEngine(onPartial, onError, "確定") },
            ask = { "返答" },
            onReply = { replies += it },
            scope = this,
            silenceMs = 60,
        )
        session.onPcm(ByteArray(320))
        delay(400)
        assertEquals(listOf("返答"), replies)
        // さらに finish() を呼んでも二重に返答しない。
        session.finish()
        assertEquals(1, replies.size)
    }

    @Test
    fun batchFallbackTranscribesAfterRecording() = runBlocking {
        val partials = mutableListOf<Pair<String, Boolean>>()
        val replies = mutableListOf<String>()
        var batchBytes = 0
        val session = DictationSession(
            id = 3,
            batchTranscribe = { bytes -> batchBytes = bytes.size; "一括の文字起こし" },
            ask = { "一括の返答" },
            onPartial = { t, f -> partials += t to f },
            onReply = { replies += it },
            scope = this,
        )
        session.onPcm(ByteArray(1000))
        assertEquals(0, partials.size) // 部分表示なし
        session.finish()
        assertEquals(1000, batchBytes)
        assertEquals(listOf("一括の文字起こし" to true), partials)
        assertEquals(listOf("一括の返答"), replies)
    }

    @Test
    fun noKeysRepliesGuidance() = runBlocking {
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 4,
            onReply = { replies += it },
            scope = this,
        )
        session.onPcm(ByteArray(100))
        session.finish()
        assertEquals(listOf(AgentEngine.NO_KEY_MESSAGE), replies)
    }

    @Test
    fun sttErrorIsReported() = runBlocking {
        var fake: FakeSttEngine? = null
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 5,
            createStt = { onPartial, onError ->
                FakeSttEngine(onPartial, onError, finalText = null).also { fake = it }
            },
            ask = { "返答" },
            onReply = { replies += it },
            scope = this,
        )
        session.start()
        fake!!.fail("音声認識に接続できませんでした")
        session.finish()
        assertEquals(listOf("音声認識に接続できませんでした"), replies)
    }

    @Test
    fun silenceTimeoutIsFiveSecondsNotThree() {
        // 発話の途中で切らない: 確定は停止 (BULK_END)/切断/5秒無音のみ。
        assertEquals(5_000L, DictationSession.SILENCE_TIMEOUT_MS)
        assertTrue(DictationSession.SILENCE_TIMEOUT_MS > 3_000L)
    }

    @Test
    fun continuousAudioIsNotFinalizedEarly() = runBlocking {
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 9,
            createStt = { onPartial, onError -> FakeSttEngine(onPartial, onError) },
            ask = { "返答" },
            onReply = { replies += it },
            scope = this,
            silenceMs = 200,
        )
        // 200ms の無音タイマーより短い間隔で 8 回 (計 800ms) 音声を送り続ける。
        repeat(8) {
            session.onPcm(ByteArray(640))
            delay(100)
        }
        assertEquals(0, replies.size)  // 話している間は確定しない
        delay(400)  // 話し終わって無音 200ms 超
        assertEquals(1, replies.size)
    }

    @Test
    fun batchFallbackBufferIsBoundedInMemory() = runBlocking {
        var batchBytes = 0
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 10,
            batchTranscribe = { bytes -> batchBytes = bytes.size; "文字起こし" },
            ask = { "返答" },
            onReply = { replies += it },
            scope = this,
        )
        // 上限 (16MB) を超えて供給してもメモリは上限で止まる (ディスクには書かない)。
        repeat(17) { session.onPcm(ByteArray(1024 * 1024)) }
        session.finish()
        assertTrue(batchBytes <= DictationSession.MAX_BATCH_PCM_BYTES)
        assertTrue(session.stats().batchTruncated)
        assertEquals(1, replies.size)
    }

    @Test
    fun voskModelMissingMessageIsReported() = runBlocking {
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 7,
            onReply = { replies += it },
            scope = this,
            notReadyMessage = AiRouting.VOSK_MODEL_MISSING,
        )
        session.onPcm(ByteArray(320))
        session.finish()
        assertEquals(listOf(AiRouting.VOSK_MODEL_MISSING), replies)
    }

    @Test
    fun sttFactoryFailureIsReported() = runBlocking {
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 8,
            createStt = { _, _ -> throw IllegalStateException(AiRouting.VOSK_MODEL_MISSING) },
            onReply = { replies += it },
            scope = this,
        )
        session.onPcm(ByteArray(320))
        session.finish()
        assertEquals(listOf(AiRouting.VOSK_MODEL_MISSING), replies)
    }

    @Test
    fun cancelDoesNotReply() = runBlocking {
        val replies = mutableListOf<String>()
        val session = DictationSession(
            id = 6,
            createStt = { onPartial, onError -> FakeSttEngine(onPartial, onError) },
            ask = { "返答" },
            onReply = { replies += it },
            scope = this,
        )
        session.onPcm(ByteArray(100))
        session.cancel()
        session.finish()
        assertEquals(0, replies.size)
    }
}
