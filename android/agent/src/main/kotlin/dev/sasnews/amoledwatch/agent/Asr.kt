package dev.sasnews.amoledwatch.agent

import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.util.UUID
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString.Companion.toByteString

/**
 * Meta Model API の Muse Voice Transcribe (リアルタイム ASR)。
 * docs/muse-integration.md §3.1 / protocol-v1.md §Agent の実装。
 *
 * - `wss://api.meta.ai/v1/asr/realtime?sessionId=<任意>` に接続し、10 秒以内に
 *   handshake JSON をテキストフレームで送る (`Authorization` ヘッダは見ない)。
 * - 音声は signed 16bit LE / モノラル / 16kHz のバイナリフレーム。
 *   **実時間ペース**で送る (5 秒以上先読みすると 1008 で切られる)。
 * - 終了は送信を drain して `{"type":"endStream"}` → サーバが 1000 で閉じるまで読む。
 */
object AsrHandshake {
    const val ENDPOINT = "wss://api.meta.ai/v1/asr/realtime"
    const val MODEL = "muse-voice-transcribe-1.0"
    const val AUDIO_ENCODING = "PCM_16KHZ"
    const val MODE = "PUSH_TO_TALK"
    const val PARTIAL_MODE = "CUMULATIVE"

    fun endpoint(sessionId: String = UUID.randomUUID().toString()): String =
        "$ENDPOINT?sessionId=$sessionId"

    fun build(
        apiKey: String,
        languageBias: List<String> = listOf("Japanese"),
        keywords: List<String> = emptyList(),
    ): JsonObject = JsonObject().apply {
        add(
            "authorization",
            JsonObject().apply { addProperty("accessToken", bearer(apiKey)) },
        )
        addProperty("audioEncoding", AUDIO_ENCODING)
        addProperty("model", MODEL)
        addProperty("mode", MODE)
        addProperty("partialMode", PARTIAL_MODE)
        if (languageBias.isNotEmpty()) {
            add("languageBias", JsonArray().apply { languageBias.forEach { add(it) } })
        }
        if (keywords.isNotEmpty()) {
            add("keywords", JsonArray().apply { keywords.forEach { add(it) } })
        }
        addProperty("emitAudioProgress", false)
    }

    fun buildFrom(config: AgentConfig): JsonObject =
        build(config.metaApiKey, config.languageBias, config.asrKeywords)

    /** `LLM_...` でも `Bearer LLM_...` でも同じ handshake になるようにする。 */
    private fun bearer(key: String): String {
        val k = key.trim()
        return if (k.startsWith("Bearer ", ignoreCase = true)) k else "Bearer $k"
    }
}

/** ASR サーバイベント (JSON テキストフレーム) の解釈結果。 */
sealed interface AsrEvent {
    /** 部分/確定 transcript。 */
    data class Transcript(val text: String, val final: Boolean) : AsrEvent

    /** speechStart / speechEnd / speechComplete (ENDPOINTING 時)。 */
    data class Lifecycle(val name: String) : AsrEvent

    data class ServerError(val message: String) : AsrEvent

    data object KeepAlive : AsrEvent

    /** 未知イベント (無視する)。 */
    data object Unknown : AsrEvent
}

/** ASR のサーバイベント JSON を解釈する (純 Kotlin)。未知イベントは Unknown。 */
object AsrEventParser {
    fun parse(json: String): AsrEvent? {
        val o = try {
            JsonParser.parseString(json) as? JsonObject
        } catch (e: Exception) {
            null
        } ?: return null
        val type = o.get("type")?.takeIf { !it.isJsonNull }?.asString ?: return AsrEvent.Unknown
        return when (type) {
            "transcript" -> {
                val text = o.get("transcript")?.takeIf { !it.isJsonNull }?.asString
                    ?: o.get("text")?.takeIf { !it.isJsonNull }?.asString
                    ?: ""
                val final = o.get("final")?.takeIf { !it.isJsonNull }?.asBoolean ?: false
                AsrEvent.Transcript(text, final)
            }
            "speechStart", "speechEnd", "speechComplete" -> AsrEvent.Lifecycle(type)
            "error" -> AsrEvent.ServerError(
                o.get("message")?.takeIf { !it.isJsonNull }?.asString ?: "ASR エラー",
            )
            "keepalive" -> AsrEvent.KeepAlive
            else -> AsrEvent.Unknown
        }
    }
}

/** リアルタイム STT の抽象 (Meta realtime / Vosk / Fake を差し替え可能)。 */
interface SttEngine {
    /** 最後に受けた部分結果。 */
    val partialText: String

    /** エラー時の日本語メッセージ (なければ null)。 */
    val errorText: String?

    /** キュー溢れで捨てた PCM バイト数 (スループットのログ用)。 */
    val droppedBytes: Long

    fun start()

    /** PCM16LE 16kHz mono を積む (実時間ペースは内部で調整)。 */
    fun feed(pcm: ByteArray)

    /** 送信を drain して endStream → close 1000 を待ち、確定 transcript を返す。 */
    suspend fun finish(timeoutMs: Long): String?

    fun cancel()
}

/**
 * OkHttp WebSocket による Meta realtime ASR の実装。
 * ASR への音声は内部キュー (最大 [MAX_QUEUE_BYTES]) に積み、実時間ペースで送出する。
 */
class MetaRealtimeStt(
    private val apiKey: String,
    private val languageBias: List<String> = listOf("Japanese"),
    private val keywords: List<String> = emptyList(),
    private val endpoint: String = AsrHandshake.endpoint(),
    private val onPartial: (String) -> Unit = {},
    private val onError: (String) -> Unit = {},
    private val client: OkHttpClient = defaultClient(),
) : SttEngine {

    companion object {
        /** 100ms 分の PCM (16kHz × 2B × 0.1s)。 */
        const val FRAME_BYTES = 3_200

        /** キューに許す最大音声 (約 2 秒)。溢れたら古い方から捨てる。 */
        const val MAX_QUEUE_BYTES = 64_000

        const val BYTES_PER_SEC = 32_000L

        /** 日本語のエラー文言。 */
        const val CONNECT_ERROR = "音声認識に接続できませんでした"

        fun defaultClient(): OkHttpClient = OkHttpClient.Builder()
            .callTimeout(0, TimeUnit.MILLISECONDS)
            .readTimeout(0, TimeUnit.MILLISECONDS)
            .build()
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val opened = CompletableDeferred<Boolean>()
    private val closedSignal = CompletableDeferred<Unit>()
    private val endSent = CompletableDeferred<Unit>()

    private val lock = Any()
    private val queue = ArrayDeque<ByteArray>()
    private var queuedBytes = 0

    @Volatile private var socket: WebSocket? = null
    @Volatile private var finishing = false
    @Volatile private var finalText: String? = null
    @Volatile private var failure: String? = null

    override var partialText: String = ""
        private set

    override val errorText: String? get() = failure

    @Volatile private var dropped = 0L
    override val droppedBytes: Long get() = dropped

    private val listener = object : WebSocketListener() {
        override fun onOpen(webSocket: WebSocket, response: Response) {
            webSocket.send(AsrHandshake.build(apiKey, languageBias, keywords).toString())
            opened.complete(true)
            scope.launch { sendLoop(webSocket) }
        }

        override fun onMessage(webSocket: WebSocket, text: String) {
            when (val evt = AsrEventParser.parse(text)) {
                is AsrEvent.Transcript -> {
                    if (evt.final) {
                        finalText = evt.text
                    } else {
                        partialText = evt.text
                        onPartial(evt.text)
                    }
                }
                is AsrEvent.ServerError -> {
                    failure = "音声認識エラー: ${evt.message}"
                    onError(failure!!)
                }
                else -> {}
            }
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
            webSocket.close(1000, null)
        }

        override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
            closedSignal.complete(Unit)
        }

        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
            if (!opened.isCompleted) opened.complete(false)
            if (failure == null) {
                failure = CONNECT_ERROR
                onError(failure!!)
            }
            closedSignal.complete(Unit)
        }
    }

    override fun start() {
        if (socket != null) return
        scope.launch {
            val req = Request.Builder().url(endpoint).build()
            socket = client.newWebSocket(req, listener)
        }
    }

    override fun feed(pcm: ByteArray) {
        if (pcm.isEmpty() || finishing) return
        synchronized(lock) {
            queue.addLast(pcm)
            queuedBytes += pcm.size
            // 溢れたら古い方から捨てる (リアルタイム優先)。
            while (queuedBytes > MAX_QUEUE_BYTES && queue.isNotEmpty()) {
                val old = queue.removeFirst()
                queuedBytes -= old.size
                dropped += old.size
            }
        }
    }

    override suspend fun finish(timeoutMs: Long): String? {
        finishing = true
        val ok = withTimeoutOrNull(timeoutMs) { opened.await() } ?: false
        if (!ok) {
            cancel()
            return finalText
        }
        // 送信ループが残りを drain して endStream を送る。
        val sent = withTimeoutOrNull(timeoutMs) { endSent.await() } != null
        if (sent) {
            // サーバが 1000 で閉じるのを待つ (final transcript は close 前に来る)。
            withTimeoutOrNull(timeoutMs) { closedSignal.await() }
        }
        val text = finalText ?: partialText.ifBlank { null }
        cancel()
        return text
    }

    override fun cancel() {
        finishing = true
        synchronized(lock) {
            queue.clear()
            queuedBytes = 0
        }
        try {
            socket?.close(1000, "done")
        } catch (e: Exception) {
        }
        scope.cancel()
    }

    // ---------------- 送信ループ (実時間ペース) ----------------

    private suspend fun sendLoop(ws: WebSocket) {
        val t0 = System.nanoTime()
        var sentBytes = 0L
        while (scope.isActive) {
            val chunk = pollUpTo(FRAME_BYTES)
            if (chunk == null) {
                if (finishing) break
                delay(5)
                continue
            }
            // 送信済みバイト数から「本来の時刻」を計算し、先読みしないように待つ。
            val targetNanos = t0 + sentBytes * 1_000_000_000L / BYTES_PER_SEC
            val waitMs = (targetNanos - System.nanoTime()) / 1_000_000
            if (waitMs > 2) delay(waitMs)
            if (!ws.send(chunk.toByteString())) break
            sentBytes += chunk.size
        }
        try {
            ws.send("{\"type\":\"endStream\"}")
        } catch (e: Exception) {
        }
        endSent.complete(Unit)
    }

    /** キューから最大 max バイト取り出す。空なら null。 */
    private fun pollUpTo(max: Int): ByteArray? = synchronized(lock) {
        if (queue.isEmpty()) return null
        if (queue.first().size <= max) {
            val a = queue.removeFirst()
            queuedBytes -= a.size
            return a
        }
        // 先頭が大きい場合は分割する。
        val first = queue.removeFirst()
        val n = minOf(max, first.size)
        val head = first.copyOfRange(0, n)
        val rest = first.copyOfRange(n, first.size)
        queue.addFirst(rest)
        queuedBytes -= n
        head
    }
}

/** ASR の確定待ちの既定タイムアウト (endStream → close)。 */
const val ASR_FINAL_TIMEOUT_MS = 4_000L
