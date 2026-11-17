package dev.sasnews.amoledwatch.agent

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/**
 * OpenAI 互換 chat/completions の `stream:true` (muse2api の SSE) のテスト。
 * サーバーが JSON を返した場合は従来の非ストリームとして読む (フォールバック)。
 */
class AgentApiStreamTest {

    private lateinit var server: MockWebServer
    private lateinit var config: AgentConfig

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
        config = AgentConfig(
            baseUrl = server.url("/v1").toString().trimEnd('/'),
            apiKey = "m2a_test",
            chatModel = "muse",
        )
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    @Test
    fun accumulatesSseDeltas() = runBlocking {
        server.enqueue(
            MockResponse()
                .setHeader("Content-Type", "text/event-stream")
                .setBody(
                    """
                    data: {"choices":[{"delta":{"content":"晴れ"}}]}

                    data: {"choices":[{"delta":{"content":"です"}}]}

                    data: {"choices":[{"delta":{},"finish_reason":"stop"}]}

                    data: [DONE]

                    """.trimIndent(),
                ),
        )
        val deltas = mutableListOf<String>()
        val text = AgentApi().chat(config, "sys", emptyList(), "今日の天気は？") { deltas += it }
        assertEquals("晴れです", text)
        assertEquals(listOf("晴れ", "晴れです"), deltas)

        val req = server.takeRequest()
        assertEquals("/v1/chat/completions", req.path)
        assertEquals("Bearer m2a_test", req.getHeader("Authorization"))
        val body = req.body.readUtf8()
        assertTrue(body.contains("\"stream\":true"))
        assertTrue(body.contains("muse"))
        assertTrue(body.contains("今日の天気は？"))
    }

    @Test
    fun plainJsonResponseStillWorks() = runBlocking {
        server.enqueue(
            MockResponse()
                .setHeader("Content-Type", "application/json")
                .setBody("""{"choices":[{"message":{"role":"assistant","content":"15時に会議です"}}]}"""),
        )
        val text = AgentApi().chat(config, "sys", emptyList(), "予定は？")
        assertEquals("15時に会議です", text)
    }

    @Test
    fun emptyStreamIsAnError() = runBlocking {
        server.enqueue(
            MockResponse()
                .setHeader("Content-Type", "text/event-stream")
                .setBody("data: [DONE]\n\n"),
        )
        val e = try {
            AgentApi().chat(config, "sys", emptyList(), "q")
            null
        } catch (ex: Exception) {
            ex
        }
        assertTrue(e is java.io.IOException)
    }
}
