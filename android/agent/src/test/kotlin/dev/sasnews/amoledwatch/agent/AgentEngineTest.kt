package dev.sasnews.amoledwatch.agent

import dev.sasnews.amoledwatch.protocol.Adpcm
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** OpenAI 互換 API を MockWebServer に立てて AgentEngine を試す。 */
class AgentEngineTest {

    private lateinit var server: MockWebServer
    private lateinit var engine: AgentEngine

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
        engine = AgentEngine(
            AgentApi(),
            AgentConfig(
                baseUrl = server.url("/v1").toString().trimEnd('/'),
                apiKey = "sk-test",
            ),
        )
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    private fun chatBody(text: String): MockResponse =
        MockResponse()
            .setHeader("Content-Type", "application/json")
            .setBody(
                """{"choices":[{"message":{"role":"assistant","content":"$text"}}]}""",
            )

    @Test
    fun answerTextSendsSystemPromptAndHistory() = runBlocking {
        server.enqueue(chatBody("晴れです"))
        val r1 = engine.answerText("今日の天気は？")
        assertEquals("晴れです", r1)

        val req1 = server.takeRequest()
        assertEquals("/v1/chat/completions", req1.path)
        assertEquals("Bearer sk-test", req1.getHeader("Authorization"))
        val body1 = req1.body.readUtf8()
        assertTrue(body1.contains(AgentEngine.SYSTEM_PROMPT.substring(0, 10)))
        assertTrue(body1.contains("今日の天気は？"))
        assertTrue(body1.contains("gpt-4o-mini"))

        // 2 回目は直前の往復が messages に入る
        server.enqueue(chatBody("15時の会議です"))
        val r2 = engine.answerText("今日の予定は？")
        assertEquals("15時の会議です", r2)
        val body2 = server.takeRequest().body.readUtf8()
        assertTrue(body2.contains("今日の天気は？"))
        assertTrue(body2.contains("晴れです"))
        assertTrue(body2.contains("今日の予定は？"))
        assertEquals(2, engine.history.size)
    }

    @Test
    fun noApiKeyReturnsGuidance() = runBlocking {
        engine.config = AgentConfig(baseUrl = server.url("/v1").toString(), apiKey = "")
        assertEquals(AgentEngine.NO_KEY_MESSAGE, engine.answerText("なにか"))
        engine.config = null
        assertEquals(AgentEngine.NO_KEY_MESSAGE, engine.answerText("なにか"))
    }

    @Test
    fun answerAudioTranscribesThenChats() = runBlocking {
        server.enqueue(
            MockResponse()
                .setHeader("Content-Type", "application/json")
                .setBody("""{"text":"今の天気は？"}"""),
        )
        server.enqueue(chatBody("晴れです"))

        val pcm = ShortArray(Adpcm.SAMPLE_RATE) { 0 }
        val reply = engine.answerAudio(Adpcm.pcmToAdp1(pcm))
        assertEquals("晴れです", reply)

        val sttReq = server.takeRequest()
        assertEquals("/v1/audio/transcriptions", sttReq.path)
        val sttBody = sttReq.body.readUtf8()
        assertTrue(sttBody.contains("whisper-1"))
        assertTrue(sttBody.contains("agent.wav"))

        val chatReq = server.takeRequest()
        assertTrue(chatReq.body.readUtf8().contains("今の天気は？"))
        assertEquals("今の天気は？", engine.history[0].question)
    }

    @Test
    fun badAudioReturnsMessage() = runBlocking {
        assertEquals("音声の形式が壊れています", engine.answerAudio(byteArrayOf(1, 2, 3)))
    }

    @Test
    fun historyIsCappedAt10() = runBlocking {
        repeat(12) { i ->
            server.enqueue(chatBody("a$i"))
            engine.answerText("q$i")
        }
        assertEquals(AgentEngine.HISTORY_MAX, engine.history.size)
        assertEquals("q2", engine.history.first().question)
    }

    @Test
    fun httpErrorReturnsJapaneseError() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(500))
        assertEquals("通信エラーです", engine.answerText("q"))
        assertEquals(0, engine.history.size)  // 失敗は履歴に残さない
    }

    @Test
    fun truncateUtf8KeepsWholeChars() {
        val s = "あ".repeat(400)  // 3bytes/char → 1200B
        val t = truncateUtf8(s, 960)
        assertEquals(320, t.length)  // 320文字×3B=960B ちょうど
        assertEquals(960, t.toByteArray(Charsets.UTF_8).size)
        val t2 = truncateUtf8(s, 961)  // 中途半端は切り捨て
        assertEquals(960, t2.toByteArray(Charsets.UTF_8).size)
        assertEquals("abc", truncateUtf8("abc", 960))
    }
}
