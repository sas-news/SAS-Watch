package dev.sasnews.amoledwatch.agent

import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

/** Muse (useapi.net) クライアント: リクエスト JSON / SSE / 再接続 / エラーのテスト。 */
class MuseTest {

    private lateinit var server: MockWebServer
    private lateinit var base: String

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
        base = server.url("/v1/muse").toString().trimEnd('/')
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    private fun sse(body: String): MockResponse = MockResponse()
        .setResponseCode(200)
        .setHeader("Content-Type", "text/event-stream")
        // 最後のイベントも空行で区切る (trimIndent が末尾の空行を落とすため)。
        .setBody(body + "\n\n")

    @Test
    fun requestBodiesAndUrlEncoding() {
        val body = MuseRequests.createChatBody("やあ", "you@example.com")
        val o = com.google.gson.JsonParser.parseString(body).asJsonObject
        assertEquals("やあ", o.get("message").asString)
        assertEquals("you@example.com", o.get("email").asString)

        val m = com.google.gson.JsonParser.parseString(MuseRequests.messageBody("次")).asJsonObject
        assertEquals("次", m.get("message").asString)
        assertNull(m.get("email"))

        val chatId = "c-u1-email:you@example.com-chat:abc-bot:muse"
        val messages = MuseRequests.messagesUrl(chatId, base)
        assertTrue(messages.contains("c-u1-email%3Ayou%40example.com-chat%3Aabc-bot%3Amuse"))
        assertTrue(messages.endsWith("/messages"))
        assertTrue(MuseRequests.streamUrl(chatId, base).endsWith("/stream"))
        assertTrue(MuseRequests.streamUrl(chatId, base, "10").contains("after=10"))
    }

    @Test
    fun streamParserHandlesAllEventTypes() {
        val p = MuseStreamParser
        assertTrue(p.parseJson("""{"event":"text","messageId":"a","text":"x"}""") is MuseEvent.Text)
        val full = p.parseJson("""{"event":"text","messageId":"a","text":"全文","full":true}""")
        assertTrue((full as MuseEvent.Text).full)
        assertTrue(p.parseJson("""{"event":"turn.done","status":"completed"}""") is MuseEvent.TurnDone)
        assertTrue(p.parseJson("""{"event":"keepalive"}""") is MuseEvent.KeepAlive)
        assertTrue(p.parseJson("""{"event":"error","code":503,"message":"x"}""") is MuseEvent.Error)
        val ap = p.parseJson(
            """{"event":"approval","approvalId":"ap1","title":"t","question":"q",
               "options":[{"decision":"allow_once","label":"Allow once"},
                          {"decision":"deny","label":"Deny"}]}""",
        )
        assertTrue(ap is MuseEvent.Approval)
        assertEquals("allow_once", (ap as MuseEvent.Approval).options[0].first)
        // 未知イベント / 壊れた JSON は落ちない。
        assertTrue(p.parseJson("""{"event":"widget","widgetId":"w"}""") is MuseEvent.Other)
        assertNull(p.parseJson("not json"))
    }

    @Test
    fun accumulatorAppendsDeltasAndReplacesOnFull() {
        val acc = MuseStreamAccumulator()
        acc.accept(MuseEvent.Text("a", "こん", false))
        acc.accept(MuseEvent.Text("a", "にちは", false))
        assertEquals("こんにちは", acc.textOf("a"))
        // full:true は全文で置き換え (再接続の replay)。
        acc.accept(MuseEvent.Text("a", "こんにちは、世界", true))
        assertEquals("こんにちは、世界", acc.textOf("a"))
        acc.accept(MuseEvent.Text("b", "別メッセージ", false))
        assertEquals("こんにちは、世界\n\n別メッセージ", acc.fullAnswer())
    }

    @Test
    fun newChatStreamsReplyAndSavesChatId() = runBlocking {
        val chatId = "c-u1-email:you@example.com-chat:abc-bot:muse"
        server.enqueue(
            MockResponse().setResponseCode(201)
                .setBody("""{"chatId":"$chatId","email":"you@example.com","messageId":"m1"}"""),
        )
        server.enqueue(
            sse(
                """
                data: {"event":"initialized","chatId":"$chatId","after":0}

                data: {"event":"status","status":"running"}

                id: 5
                data: {"event":"text","messageId":"a1","text":"こん"}

                data: {"event":"text","messageId":"a1","text":"にちは"}

                id: 6
                data: {"event":"turn.done","status":"completed"}

                """.trimIndent(),
            ),
        )
        val service = MuseReplyService(MuseApi(baseUrl = base))
        val cfg = AgentConfig(
            museEnabled = true,
            museToken = "user:12345-abc",
            museEmail = "you@example.com",
        )
        val deltas = mutableListOf<String>()
        var savedChatId: String? = null
        val r = service.ask(cfg, "やあ", onChatId = { savedChatId = it }, onDelta = { deltas += it })
        assertEquals("こんにちは", r.text)
        assertEquals(chatId, savedChatId)
        assertEquals(chatId, r.chatId)
        assertTrue(deltas.last().contains("こんにちは"))

        val create = server.takeRequest()
        assertEquals("POST", create.method)
        assertTrue(create.path!!.endsWith("/chats"))
        assertEquals("Bearer user:12345-abc", create.getHeader("Authorization"))
        val createBody = create.body.readUtf8()
        assertTrue(createBody.contains("やあ"))
        assertTrue(createBody.contains("you@example.com"))

        val stream = server.takeRequest()
        assertEquals("GET", stream.method)
        assertTrue(stream.path!!.contains("/chats/"))
        assertTrue(stream.path!!.endsWith("/stream"))
        assertNull(stream.getHeader("Last-Event-ID"))
    }

    @Test
    fun existingChatReconnectsWithLastEventIdAndUsesFullText() = runBlocking {
        val chatId = "c-u1-email:you@example.com-chat:abc-bot:muse"
        server.enqueue(
            MockResponse().setResponseCode(202)
                .setBody("""{"chatId":"$chatId","email":"you@example.com","messageId":"m2"}"""),
        )
        // 1 回目: 差分の途中で error → 再接続。
        server.enqueue(
            sse(
                """
                id: 10
                data: {"event":"text","messageId":"a1","text":"部分"}

                data: {"event":"error","code":503,"message":"dropped"}

                """.trimIndent(),
            ),
        )
        // 2 回目: replay の full:true で全文が来て turn.done。
        server.enqueue(
            sse(
                """
                id: 11
                data: {"event":"text","messageId":"a1","text":"全文です","full":true}

                id: 12
                data: {"event":"turn.done","status":"completed"}

                """.trimIndent(),
            ),
        )
        val service = MuseReplyService(MuseApi(baseUrl = base))
        val cfg = AgentConfig(
            museEnabled = true,
            museToken = "user:12345-abc",
            museChatId = chatId,
        )
        val r = service.ask(cfg, "続き")
        assertEquals("全文です", r.text)

        assertEquals("POST", server.takeRequest().method)
        val firstStream = server.takeRequest()
        assertNull(firstStream.getHeader("Last-Event-ID"))
        val secondStream = server.takeRequest()
        assertEquals("10", secondStream.getHeader("Last-Event-ID"))
    }

    @Test
    fun approvalPausesAndReturnsGuidance() = runBlocking {
        val chatId = "c-u1-email:you@example.com-chat:abc-bot:muse"
        server.enqueue(
            MockResponse().setResponseCode(202)
                .setBody("""{"chatId":"$chatId","messageId":"m3"}"""),
        )
        server.enqueue(
            sse(
                """
                data: {"event":"status","status":"pending_user_confirmation"}

                id: 20
                data: {"event":"approval","approvalId":"ap1","title":"Share info","question":"Allow Muse to share information?","options":[{"decision":"allow_once","label":"Allow once"},{"decision":"deny","label":"Deny"}]}

                """.trimIndent(),
            ),
        )
        val service = MuseReplyService(MuseApi(baseUrl = base))
        var approval: MuseApproval? = null
        val r = service.ask(
            AgentConfig(museEnabled = true, museToken = "user:1-x", museChatId = chatId),
            "これを共有して",
            onApproval = { approval = it },
        )
        assertEquals(MuseApi.APPROVAL_MESSAGE, r.text)
        assertNotNull(approval)
        assertEquals("ap1", approval!!.approvalId)
        assertEquals("allow_once", approval!!.options[0].first)
    }

    @Test
    fun rateLimitBecomesJapaneseError() = runBlocking {
        server.enqueue(
            MockResponse().setResponseCode(429)
                .setBody("""{"error":"Muse is limiting requests","code":429}"""),
        )
        val service = MuseReplyService(MuseApi(baseUrl = base))
        val e = try {
            service.ask(
                AgentConfig(museEnabled = true, museToken = "user:1-x", museChatId = "c-x-y"),
                "q",
            )
            null
        } catch (ex: MuseException) {
            ex
        }
        assertNotNull(e)
        assertTrue(e!!.japanese.contains("レート制限"))
        assertTrue(e.japanese.contains("Muse is limiting"))
    }

    @Test
    fun missingMuseTokenReturnsNoKeyMessage() = runBlocking {
        val service = MuseReplyService(MuseApi(baseUrl = base))
        val e = try {
            service.ask(AgentConfig(museEnabled = false), "q")
            null
        } catch (ex: MuseException) {
            ex
        }
        assertEquals(MuseApi.MUSE_TOKEN_MISSING, e!!.japanese)
        assertEquals(0, server.requestCount)
    }

    @Test
    fun listsAndReadsChats() = runBlocking {
        server.enqueue(
            MockResponse().setResponseCode(200).setBody(
                """{"email":"you@example.com","chats":[
                     {"chatId":"c-1","title":"海の話","snippet":"...","status":"completed",
                      "created":"2026-10-12T00:00:00Z","updated":"2026-10-12T01:00:00Z"},
                     {"chatId":"c-2","title":null,"snippet":null,"status":"running"}
                   ],"cursor":null}""",
            ),
        )
        server.enqueue(
            MockResponse().setResponseCode(200).setBody(
                """{"chatId":"c-1","title":"海の話","messages":[
                     {"messageId":"m1","role":"user","text":"海の話をして","files":[]},
                     {"messageId":"m2","role":"assistant","text":"昔々…",
                      "files":[{"assetId":"asset-1","type":"image","mime":"image/webp","bytes":10,"origin":"generated"}]}
                   ]}""",
            ),
        )
        val api = MuseApi(baseUrl = base)
        val chats = api.listChats("user:1-x", "you@example.com")
        assertEquals(2, chats.size)
        assertEquals("海の話", chats[0].title)
        assertNull(chats[1].title)
        val req = server.takeRequest()
        assertTrue(req.path!!.startsWith("/v1/muse/chats?"))
        assertTrue(req.path!!.contains("email=you%40example.com"))

        val detail = api.readChat("user:1-x", "c-1")
        assertEquals("海の話", detail.title)
        assertEquals(2, detail.messages.size)
        assertEquals("assistant", detail.messages[1].role)
        assertEquals(listOf("asset-1"), detail.messages[1].files)
    }
}
