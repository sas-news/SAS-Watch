package dev.sasnews.amoledwatch.agent

import com.google.gson.JsonParser
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import okhttp3.OkHttpClient
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** Meta realtime ASR (Muse Voice Transcribe) の handshake / イベント / WebSocket のテスト。 */
class AsrTest {

    private lateinit var server: MockWebServer

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    @Test
    fun handshakeMatchesDesignDoc() {
        val o = AsrHandshake.build(
            "LLM_test",
            languageBias = listOf("Japanese"),
            keywords = listOf("SAS-Watch", "Muse"),
        )
        assertEquals(
            "Bearer LLM_test",
            o.getAsJsonObject("authorization").get("accessToken").asString,
        )
        assertEquals("PCM_16KHZ", o.get("audioEncoding").asString)
        assertEquals("muse-voice-transcribe-1.0", o.get("model").asString)
        assertEquals("PUSH_TO_TALK", o.get("mode").asString)
        assertEquals("CUMULATIVE", o.get("partialMode").asString)
        assertEquals("Japanese", o.getAsJsonArray("languageBias")[0].asString)
        assertEquals("SAS-Watch", o.getAsJsonArray("keywords")[0].asString)
        assertEquals(false, o.get("emitAudioProgress").asBoolean)

        // Bearer が二重にならない / キーワード空なら配列を送らない。
        val o2 = AsrHandshake.buildFrom(
            AgentConfig(metaApiKey = "Bearer LLM_x", asrKeywords = emptyList()),
        )
        assertEquals("Bearer LLM_x", o2.getAsJsonObject("authorization").get("accessToken").asString)
        assertNull(o2.get("keywords"))

        assertTrue(AsrHandshake.endpoint("s1").startsWith("wss://api.meta.ai/v1/asr/realtime?sessionId=s1"))
    }

    @Test
    fun parsesServerEvents() {
        val partial = AsrEventParser.parse(
            """{"type":"transcript","transcript":"こんにち","final":false}""",
        )
        assertTrue(partial is AsrEvent.Transcript)
        assertEquals("こんにち", (partial as AsrEvent.Transcript).text)
        assertEquals(false, partial.final)

        val fin = AsrEventParser.parse("""{"type":"transcript","text":"こんにちは","final":true}""")
        assertTrue(fin is AsrEvent.Transcript)
        assertEquals("こんにちは", (fin as AsrEvent.Transcript).text)
        assertEquals(true, fin.final)

        assertTrue(AsrEventParser.parse("""{"type":"speechComplete"}""") is AsrEvent.Lifecycle)
        assertTrue(AsrEventParser.parse("""{"type":"keepalive"}""") is AsrEvent.KeepAlive)
        assertTrue(AsrEventParser.parse("""{"type":"error","message":"x"}""") is AsrEvent.ServerError)
        // 未知イベント / 壊れた JSON は落ちない。
        assertTrue(AsrEventParser.parse("""{"type":"somethingNew"}""") is AsrEvent.Unknown)
        assertNull(AsrEventParser.parse("not json"))
    }

    @Test
    fun websocketSendsHandshakeAndPacedAudioThenEndStream() = runBlocking {
        val handshake = java.util.concurrent.atomic.AtomicReference<String>()
        val audioBytes = java.util.concurrent.atomic.AtomicInteger()
        val endStream = java.util.concurrent.atomic.AtomicBoolean()
        val serverListener = object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                when {
                    text.contains("endStream") -> {
                        endStream.set(true)
                        webSocket.close(1000, "done")
                    }
                    text.contains("accessToken") -> {
                        handshake.set(text)
                        // 部分結果を 1 つ返す (handshake の直後に届く想定)。
                        webSocket.send(
                            """{"type":"transcript","transcript":"こんにち","final":false}""",
                        )
                    }
                }
            }

            override fun onMessage(webSocket: WebSocket, bytes: okio.ByteString) {
                audioBytes.addAndGet(bytes.size)
                // 最後の音声で確定 transcript を返す。
                webSocket.send(
                    """{"type":"transcript","transcript":"こんにちは","final":true}""",
                )
            }
        }
        server.enqueue(MockResponse().withWebSocketUpgrade(serverListener))

        val partials = mutableListOf<String>()
        val wsUrl = server.url("/v1/asr/realtime?sessionId=test")
            .toString().replaceFirst("http://", "ws://")
        val stt = MetaRealtimeStt(
            apiKey = "LLM_test",
            languageBias = listOf("Japanese"),
            keywords = listOf("SAS-Watch"),
            endpoint = wsUrl,
            onPartial = { partials += it },
            client = OkHttpClient(),
        )
        stt.start()
        stt.feed(ByteArray(3_200) { 1 })
        val text = stt.finish(5_000)

        assertEquals("こんにちは", text)
        assertEquals(listOf("こんにち"), partials)
        assertEquals(3_200, audioBytes.get())
        assertTrue(endStream.get())
        assertNotNull(handshake.get())
        val h = JsonParser.parseString(handshake.get()).asJsonObject
        assertEquals("Bearer LLM_test", h.getAsJsonObject("authorization").get("accessToken").asString)
        assertEquals("muse-voice-transcribe-1.0", h.get("model").asString)
        assertEquals("PUSH_TO_TALK", h.get("mode").asString)
        assertEquals("CUMULATIVE", h.get("partialMode").asString)
        assertEquals("PCM_16KHZ", h.get("audioEncoding").asString)
        assertEquals(false, h.get("emitAudioProgress").asBoolean)
        assertEquals("Japanese", h.getAsJsonArray("languageBias")[0].asString)
        assertEquals("SAS-Watch", h.getAsJsonArray("keywords")[0].asString)
    }

    @Test
    fun queueOverflowDropsOldestAudio() = runBlocking {
        // ソケットを開かずに feed しまくるとキュー (2秒 = 64KB) が溢れる。
        val stt = MetaRealtimeStt(
            apiKey = "LLM_test",
            endpoint = "ws://127.0.0.1:1/never", // 接続しない
            client = OkHttpClient(),
        )
        repeat(30) { stt.feed(ByteArray(3_200)) } // 96,000B > 64,000B
        assertTrue(stt.droppedBytes > 0)
        stt.cancel()
        delay(1)
    }

    @Test
    fun connectionFailureReportsJapaneseError() = runBlocking {
        // 誰も待ち受けていないポートへ接続 → 日本語エラー。
        val stt = MetaRealtimeStt(
            apiKey = "LLM_test",
            endpoint = "ws://127.0.0.1:1/never",
            client = OkHttpClient(),
        )
        stt.start()
        stt.feed(ByteArray(320))
        val text = stt.finish(1_500)
        assertNull(text)
        assertEquals(MetaRealtimeStt.CONNECT_ERROR, stt.errorText)
    }
}
