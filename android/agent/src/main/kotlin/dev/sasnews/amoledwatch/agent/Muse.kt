package dev.sasnews.amoledwatch.agent

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.io.IOException
import java.net.URLEncoder
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody

/**
 * Muse (Meta personal AI agent) を useapi.net 経由で使うクライアント。
 * docs/muse-integration.md §3.2 / protocol-v1.md §Agent の実装。
 *
 * - 認証: `Authorization: Bearer user:<番号>-<文字列>`
 * - ベース: `https://api.useapi.net/v1/muse`
 * - チャット作成: POST /chats `{message,email}` → 201 `{chatId,...}`
 * - 送信: POST /chats/{urlencode(chatId)}/messages `{message}` → 202
 * - 応答: GET /chats/{urlencode(chatId)}/stream の SSE (`Last-Event-ID` で再接続)
 * - 一覧/取得: GET /chats, GET /chats/{chatId}
 */
object MuseRequests {
    const val BASE_URL = "https://api.useapi.net/v1/muse"

    fun urlEncode(value: String): String =
        URLEncoder.encode(value, "UTF-8").replace("+", "%20")

    fun createChatBody(message: String, email: String): String = JsonObject().apply {
        addProperty("message", message)
        if (email.isNotBlank()) addProperty("email", email.trim())
    }.toString()

    fun messageBody(message: String): String =
        JsonObject().apply { addProperty("message", message) }.toString()

    fun createChatUrl(baseUrl: String = BASE_URL): String = "${base(baseUrl).trimEnd('/')}/chats"

    fun chatsUrl(baseUrl: String = BASE_URL, email: String? = null, limit: Int = 50): String {
        val q = StringBuilder()
        if (!email.isNullOrBlank()) q.append("email=").append(urlEncode(email))
        if (limit > 0) {
            if (q.isNotEmpty()) q.append('&')
            q.append("limit=").append(limit)
        }
        val s = q.toString()
        return "${base(baseUrl)}/chats" + if (s.isEmpty()) "" else "?$s"
    }

    fun chatUrl(chatId: String, baseUrl: String = BASE_URL): String =
        "${base(baseUrl)}/chats/${urlEncode(chatId)}"

    fun messagesUrl(chatId: String, baseUrl: String = BASE_URL): String =
        "${chatUrl(chatId, baseUrl)}/messages"

    fun streamUrl(chatId: String, baseUrl: String = BASE_URL, after: String? = null): String {
        val u = "${chatUrl(chatId, baseUrl)}/stream"
        return if (after.isNullOrBlank()) u else "$u?after=${urlEncode(after)}"
    }

    private fun base(baseUrl: String = BASE_URL): String =
        if (baseUrl.isBlank()) BASE_URL else baseUrl
}

/** Muse のストリーム (SSE の data JSON) を解釈したイベント。 */
sealed interface MuseEvent {
    data class Initialized(val chatId: String, val after: Long) : MuseEvent

    /** エージェントの本文。full=true は全文 (置き換え)、false は差分 (連結)。 */
    data class Text(val messageId: String, val text: String, val full: Boolean) : MuseEvent

    data class MessageDone(val messageId: String) : MuseEvent

    /** エージェントが今していること (例: "Generating image")。 */
    data class Activity(val text: String) : MuseEvent

    data class Status(val status: String) : MuseEvent

    /** 承認要求。v1 は時計では承認できないので「スマホで承認が必要」に誘導する。 */
    data class Approval(
        val approvalId: String,
        val title: String?,
        val question: String?,
        val options: List<Pair<String, String>>,
    ) : MuseEvent

    data class ApprovalResolved(val approvalId: String, val status: String) : MuseEvent

    /** 回答完了。status は completed / interrupted。 */
    data class TurnDone(val status: String) : MuseEvent

    /** 接続が切れた (Last-Event-ID で再接続できる)。 */
    data class Error(val code: Int, val message: String) : MuseEvent

    data object KeepAlive : MuseEvent

    data class Reconnect(val message: String) : MuseEvent

    data class Idle(val message: String) : MuseEvent

    data class Other(val name: String) : MuseEvent
}

/** SSE の 1 イベントを [MuseEvent] にする。data JSON の `event` フィールドを使う。 */
object MuseStreamParser {
    fun parse(event: SseEvent): MuseEvent? = try {
        parseJson(event.data, event.event)
    } catch (e: Exception) {
        null
    }

    fun parseJson(data: String, fallbackName: String? = null): MuseEvent? {
        if (data.isBlank()) return null
        val o = try {
            JsonParser.parseString(data) as? JsonObject
        } catch (e: Exception) {
            null
        } ?: return null
        val name = o.str("event") ?: fallbackName ?: return null
        return when (name) {
            "initialized" -> MuseEvent.Initialized(
                o.str("chatId") ?: "",
                o["after"]?.asLongOrNull() ?: 0L,
            )
            "text" -> MuseEvent.Text(
                o.str("messageId") ?: "",
                o.str("text") ?: "",
                o.bool("full") ?: false,
            )
            "message.done" -> MuseEvent.MessageDone(o.str("messageId") ?: "")
            "activity" -> MuseEvent.Activity(o.str("text") ?: "")
            "status" -> MuseEvent.Status(o.str("status") ?: "")
            "approval" -> MuseEvent.Approval(
                approvalId = o.str("approvalId") ?: "",
                title = o.str("title"),
                question = o.str("question"),
                options = o.arr("options").mapNotNull { it as? JsonObject }.map {
                    (it.str("decision") ?: "") to (it.str("label") ?: "")
                },
            )
            "approval.resolved" -> MuseEvent.ApprovalResolved(
                o.str("approvalId") ?: "",
                o.str("status") ?: "",
            )
            "turn.done" -> MuseEvent.TurnDone(o.str("status") ?: "")
            "error" -> MuseEvent.Error(o["code"]?.asIntOrNull() ?: 0, o.str("message") ?: "")
            "keepalive" -> MuseEvent.KeepAlive
            "reconnect" -> MuseEvent.Reconnect(o.str("message") ?: "")
            "idle" -> MuseEvent.Idle(o.str("message") ?: "")
            else -> MuseEvent.Other(name)
        }
    }

    private fun JsonObject.str(name: String): String? =
        get(name)?.takeIf { !it.isJsonNull }?.asString

    private fun JsonObject.bool(name: String): Boolean? =
        get(name)?.takeIf { !it.isJsonNull }?.asBoolean

    private fun JsonObject.arr(name: String): List<JsonElement> =
        (get(name) as? JsonArray)?.toList() ?: emptyList()

    private fun com.google.gson.JsonElement.asLongOrNull(): Long? =
        runCatching { asLong }.getOrNull()

    private fun com.google.gson.JsonElement.asIntOrNull(): Int? =
        runCatching { asInt }.getOrNull()
}

/**
 * `text` イベントを messageId ごとに連結する。
 * `full:true` が来たらその messageId の全文で置き換える (再接続時の replay 対策)。
 */
class MuseStreamAccumulator {
    private val texts = LinkedHashMap<String, String>()

    fun accept(event: MuseEvent) {
        if (event is MuseEvent.Text) {
            if (event.full) {
                texts[event.messageId] = event.text
            } else {
                texts[event.messageId] = (texts[event.messageId] ?: "") + event.text
            }
        }
    }

    fun textOf(messageId: String): String = texts[messageId] ?: ""

    /** 全メッセージを順に連結した本文 (時計へ返す最終テキストの元)。 */
    fun fullAnswer(): String =
        texts.values.map { it.trim() }.filter { it.isNotEmpty() }.joinToString("\n\n")

    fun reset() = texts.clear()
}

/** GET /chats の 1 件。 */
data class MuseChatSummary(
    val chatId: String,
    val title: String?,
    val snippet: String?,
    val status: String?,
    val updated: String?,
)

/** チャット内の 1 メッセージ。role は user / assistant。 */
data class MuseChatMessage(
    val messageId: String,
    val role: String,
    val text: String,
    val files: List<String> = emptyList(),
)

/** GET /chats/{chatId} の結果。 */
data class MuseChatDetail(
    val chatId: String,
    val title: String?,
    val messages: List<MuseChatMessage>,
)

/** 承認要求の表示用。 */
data class MuseApproval(
    val approvalId: String,
    val title: String?,
    val question: String?,
    val options: List<Pair<String, String>>,
)

/** Muse の API エラー (メッセージは日本語。そのまま時計へ出せる)。 */
class MuseException(val status: Int, val japanese: String) : IOException(japanese)

/** Muse へ送ったメッセージの結果。 */
data class MuseStarted(val chatId: String, val messageId: String?)

/** streamOnce の終わり方。 */
data class MuseStreamEnd(
    /** 最後に受けた SSE の id (再接続の Last-Event-ID に使う)。 */
    val lastEventId: String?,
    /** turn.done / error / reconnect / idle / closed / stopped。 */
    val reason: String,
)

/** useapi.net Muse API の HTTP クライアント。 */
class MuseApi(
    private val client: OkHttpClient = OkHttpClient.Builder()
        .callTimeout(45, TimeUnit.SECONDS)
        .build(),
    private val baseUrl: String = MuseRequests.BASE_URL,
) {
    companion object {
        private val JSON = "application/json".toMediaType()
        const val MUSE_TOKEN_MISSING = "スマホでAI設定をしてください"
        const val APPROVAL_MESSAGE = "スマホで承認が必要です"
        const val EMPTY_REPLY = "Muse から返答がありませんでした"
        const val STREAM_BROKEN = "Muse の応答が途中で切れました"
    }

    private fun bearer(token: String) = "Bearer ${token.trim()}"

    /** チャットを新規作成して最初のメッセージを送る。 */
    suspend fun createChat(token: String, message: String, email: String): MuseStarted =
        withContext(Dispatchers.IO) {
            val req = Request.Builder()
                .url(MuseRequests.createChatUrl(baseUrl))
                .header("Authorization", bearer(token))
                .post(MuseRequests.createChatBody(message, email).toRequestBody(JSON))
                .build()
            executeJson(req, expect = 201).let {
                MuseStarted(it.str("chatId") ?: throw MuseException(0, "Muse の chatId が返りません"), it.str("messageId"))
            }
        }

    /** 既存チャットへメッセージを送る。 */
    suspend fun sendMessage(token: String, chatId: String, message: String): MuseStarted =
        withContext(Dispatchers.IO) {
            val req = Request.Builder()
                .url(MuseRequests.messagesUrl(chatId, baseUrl))
                .header("Authorization", bearer(token))
                .post(MuseRequests.messageBody(message).toRequestBody(JSON))
                .build()
            executeJson(req, expect = 202).let {
                MuseStarted(
                    it.str("chatId") ?: chatId,
                    it.str("messageId"),
                )
            }
        }

    /** チャット一覧 (新しい順)。 */
    suspend fun listChats(token: String, email: String): List<MuseChatSummary> =
        withContext(Dispatchers.IO) {
            val req = Request.Builder()
                .url(MuseRequests.chatsUrl(baseUrl, email.ifBlank { null }))
                .header("Authorization", bearer(token))
                .get()
                .build()
            val o = executeJson(req, expect = 200)
            o.arr("chats").mapNotNull { it as? JsonObject }.mapNotNull { c ->
                val id = c.str("chatId") ?: return@mapNotNull null
                MuseChatSummary(
                    chatId = id,
                    title = c.str("title"),
                    snippet = c.str("snippet"),
                    status = c.str("status"),
                    updated = c.str("updated"),
                )
            }
        }

    /** チャットのメッセージ (最新部分)。 */
    suspend fun readChat(token: String, chatId: String): MuseChatDetail =
        withContext(Dispatchers.IO) {
            val req = Request.Builder()
                .url("${MuseRequests.chatUrl(chatId, baseUrl)}?limit=100")
                .header("Authorization", bearer(token))
                .get()
                .build()
            val o = executeJson(req, expect = 200)
            val messages = o.arr("messages").mapNotNull { it as? JsonObject }.mapNotNull { m ->
                val role = m.str("role") ?: return@mapNotNull null
                MuseChatMessage(
                    messageId = m.str("messageId") ?: "",
                    role = role,
                    text = m.str("text") ?: "",
                    files = m.arr("files").mapNotNull { f ->
                        (f as? JsonObject)?.str("assetId") ?: (f as? com.google.gson.JsonPrimitive)?.asString
                    },
                )
            }
            MuseChatDetail(chatId, o.str("title"), messages)
        }

    /**
     * ストリームを 1 回読む。`onEvent` が false を返すか turn.done / error で終わる。
     * 再接続は呼び出し側 ([MuseReplyService]) が Last-Event-ID で行う。
     */
    suspend fun streamOnce(
        token: String,
        chatId: String,
        after: String?,
        onEvent: (MuseEvent) -> Boolean,
    ): MuseStreamEnd = withContext(Dispatchers.IO) {
        val streamClient = client.newBuilder()
            // ストリームは長く開く。keepalive は 15 秒ごとに来るので read 90 秒で十分。
            .readTimeout(90, TimeUnit.SECONDS)
            .callTimeout(0, TimeUnit.MILLISECONDS)
            .build()
        val req = Request.Builder()
            .url(MuseRequests.streamUrl(chatId, baseUrl, after))
            .header("Authorization", bearer(token))
            .apply { if (!after.isNullOrBlank()) header("Last-Event-ID", after) }
            .get()
            .build()
        var lastId = after
        var reason = "closed"
        try {
            streamClient.newCall(req).execute().use { res ->
                if (!res.isSuccessful) throw httpError(res.code, res.body?.string() ?: "")
                val source = res.body?.source() ?: throw MuseException(0, STREAM_BROKEN)
                val parser = SseParser()
                var running = true
                while (running) {
                    val line = source.readUtf8Line() ?: break
                    val sse = parser.feedLine(line) ?: continue
                    if (!sse.id.isNullOrBlank()) lastId = sse.id
                    val evt = MuseStreamParser.parse(sse) ?: continue
                    if (!onEvent(evt)) {
                        reason = "stopped"
                        running = false
                        break
                    }
                    when (evt) {
                        is MuseEvent.TurnDone -> {
                            reason = "turn.done"
                            running = false
                        }
                        is MuseEvent.Error -> {
                            reason = "error"
                            running = false
                        }
                        is MuseEvent.Reconnect -> {
                            reason = "reconnect"
                            running = false
                        }
                        is MuseEvent.Idle -> {
                            reason = "idle"
                            running = false
                        }
                        else -> {}
                    }
                }
            }
        } catch (e: MuseException) {
            throw e
        }
        MuseStreamEnd(lastId, reason)
    }

    private fun executeJson(req: Request, expect: Int): JsonObject {
        client.newCall(req).execute().use { res ->
            val body = res.body?.string() ?: ""
            if (!res.isSuccessful || (expect != 0 && res.code != expect)) {
                throw httpError(res.code, body)
            }
            return try {
                JsonParser.parseString(body) as? JsonObject
                    ?: throw MuseException(0, "Muse の応答が JSON ではありません")
            } catch (e: MuseException) {
                throw e
            } catch (e: Exception) {
                throw MuseException(0, "Muse の応答を読めませんでした")
            }
        }
    }

    /** HTTP ステータスを日本語のエラーにする。本文の error は補足として添える。 */
    fun httpError(status: Int, body: String): MuseException {
        val apiMsg = try {
            (JsonParser.parseString(body) as? JsonObject)?.str("error")
        } catch (e: Exception) {
            null
        }
        val ja = when (status) {
            401 -> "useapi.net のトークンが正しくありません"
            403 -> "この chatId は別のトークンのものです"
            404 -> "Muse のアカウントかチャットが見つかりません"
            412 -> "Muse の週次利用枠がなくなりました"
            429 -> "Muse のレート制限中です。少し待ってください"
            502 -> "Muse がメッセージを受け付けませんでした"
            503 -> "Muse に接続できませんでした"
            504 -> "Muse への送信中に切断されました"
            596 -> "Muse アカウントの再設定が必要です"
            else -> "Muse 通信エラー (HTTP $status)"
        }
        return MuseException(status, if (apiMsg.isNullOrBlank()) ja else "$ja（$apiMsg）")
    }

    private fun JsonObject.str(name: String): String? =
        get(name)?.takeIf { !it.isJsonNull }?.asString

    private fun JsonObject.arr(name: String): List<JsonElement> =
        (get(name) as? JsonArray)?.toList() ?: emptyList()
}

/**
 * Muse に 1 往復させる。チャットが無ければ作成し、SSE を読んで最終本文を返す。
 * ストリームが切れたら Last-Event-ID で最大 [MAX_RECONNECTS] 回まで再接続する。
 */
class MuseReplyService(
    private val api: MuseApi = MuseApi(),
) {
    companion object {
        const val MAX_RECONNECTS = 2
        const val RECONNECT_DELAY_MS = 400L
    }

    /**
     * @param onChatId 新規作成した chatId を保存するためのコールバック。
     * @param onDelta ストリーミング中の累積本文 (チャット画面の「受信中」表示用)。
     * @param onApproval 承認要求が来たときの通知。
     */
    suspend fun ask(
        config: AgentConfig,
        message: String,
        onChatId: (String) -> Unit = {},
        onDelta: (String) -> Unit = {},
        onApproval: (MuseApproval) -> Unit = {},
    ): MuseTurnResult {
        if (!config.museUsable()) throw MuseException(0, MuseApi.MUSE_TOKEN_MISSING)

        var chatId = config.museChatId.trim()
        if (chatId.isEmpty()) {
            val started = api.createChat(config.museToken, message, config.museEmail)
            chatId = started.chatId
            onChatId(chatId)
        } else {
            api.sendMessage(config.museToken, chatId, message)
        }

        val acc = MuseStreamAccumulator()
        var approval: MuseApproval? = null
        var after: String? = null
        var attempts = 0
        while (true) {
            val end = api.streamOnce(config.museToken, chatId, after) { evt ->
                acc.accept(evt)
                when (evt) {
                    is MuseEvent.Text -> onDelta(acc.fullAnswer())
                    is MuseEvent.Approval -> {
                        approval = MuseApproval(
                            evt.approvalId, evt.title, evt.question, evt.options,
                        )
                        onApproval(approval!!)
                        return@streamOnce false
                    }
                    is MuseEvent.TurnDone -> return@streamOnce false
                    else -> {}
                }
                true
            }
            after = end.lastEventId ?: after

            if (approval != null) break
            if (end.reason == "turn.done" || end.reason == "stopped" || end.reason == "closed") break
            if (end.reason != "error" && end.reason != "reconnect" && end.reason != "idle") break

            if (++attempts > MAX_RECONNECTS) throw MuseException(0, MuseApi.STREAM_BROKEN)
            delay(RECONNECT_DELAY_MS)
        }

        if (approval != null) {
            return MuseTurnResult(MuseApi.APPROVAL_MESSAGE, chatId, approval)
        }
        val text = acc.fullAnswer().trim()
        if (text.isEmpty()) return MuseTurnResult(MuseApi.EMPTY_REPLY, chatId, null)
        return MuseTurnResult(text, chatId, null)
    }
}

/** Muse への 1 往復の結果。text は表示用 (時計は呼び出し側で 960B に切る)。 */
data class MuseTurnResult(
    val text: String,
    val chatId: String,
    val approval: MuseApproval?,
)
