package dev.sasnews.amoledwatch.agent

import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody

/**
 * AI の接続設定 (設定画面「AI設定」で編集)。秘密値は AgentPrefs (EncryptedSharedPreferences) に保存。
 *
 * - OpenAI 互換: [baseUrl] / [apiKey] / [chatModel] / [sttModel] — 従来のフォールバック経路。
 * - Muse (useapi.net): [museEnabled] / [museToken] / [museChatId] / [museEmail]。
 * - Meta realtime ASR (Muse Voice Transcribe): [metaApiKey]。
 * - [sttMode] は STT 経路の選択 (`auto` / `muse_realtime` / `openai` / `none`)。
 */
data class AgentConfig(
    /**
     * 自前 muse2api (OpenAI 互換) の接続先。
     * 既定は Tailscale Serve (tailnet 内のみ・TLS) のアドレス。
     * 同じ Wi-Fi 内なら `http://192.168.10.10:18610/v1` も使える。
     * 公開 URL (`https://muse.sasnews.dev`) は既定にしない。
     */
    val baseUrl: String = "https://sas.tail85c8c5.ts.net:8445/v1",
    val apiKey: String = "",
    val chatModel: String = "muse-spark",
    val sttModel: String = "whisper-1",
    /** Muse (useapi.net) を返答に使う。OFF なら OpenAI 互換 chat/completions。 */
    val museEnabled: Boolean = false,
    /** useapi.net の API トークン (`user:12345-...`)。 */
    val museToken: String = "",
    /** 送信先の Muse chatId (空なら新規作成して保存)。 */
    val museChatId: String = "",
    /** Muse アカウントのメール (チャット作成時の email)。 */
    val museEmail: String = "",
    /** Meta Model API キー (`LLM_...`)。Muse Voice Transcribe 用。 */
    val metaApiKey: String = "",
    /** STT 方式。[SttModes] のいずれか。 */
    val sttMode: String = SttModes.AUTO,
    /** ASR の languageBias (既定: Japanese)。 */
    val languageBias: List<String> = listOf("Japanese"),
    /** ASR の keywords (誤認識しやすい固有名詞)。 */
    val asrKeywords: List<String> = emptyList(),
) {
    /** OpenAI 互換 API のキーと Base URL が揃っているか。 */
    fun usable(): Boolean = baseUrl.isNotBlank() && apiKey.isNotBlank()

    /** Meta realtime ASR を使えるか。 */
    fun metaAsrUsable(): Boolean = metaApiKey.isNotBlank()

    /** Muse (useapi.net) に送れるか。 */
    fun museUsable(): Boolean = museEnabled && museToken.isNotBlank()
}

/**
 * OpenAI 互換の薄いクライアント。
 * - STT: POST {baseUrl}/audio/transcriptions (multipart, response_format=json)
 * - 返答: POST {baseUrl}/chat/completions
 */
class AgentApi(
    private val client: OkHttpClient = OkHttpClient.Builder()
        // 時計側のタイムアウト (60秒) より早くエラーを返して具体的な文言を届ける。
        .callTimeout(45, TimeUnit.SECONDS)
        .build(),
) {

    /**
     * 一括 STT 用のクライアント。3 分の音声 (WAV ≒ 5.8MB) を送っても切れないよう
     * 転送タイムアウトを長めにする。音声はメモリ内の WAV だけで、ディスクには書かない。
     */
    private val transcribeClient: OkHttpClient =
        client.newBuilder().callTimeout(180, TimeUnit.SECONDS).build()

    /** WAV (メモリ内) を送って書き起こしテキストを返す。 */
    suspend fun transcribe(config: AgentConfig, wav: ByteArray): String =
        withContext(Dispatchers.IO) {
            val body = MultipartBody.Builder()
                .setType(MultipartBody.FORM)
                .addFormDataPart("model", config.sttModel)
                .addFormDataPart("response_format", "json")
                .addFormDataPart(
                    "file", "agent.wav",
                    wav.toRequestBody("audio/wav".toMediaType()),
                )
                .build()
            val req = Request.Builder()
                .url("${config.baseUrl.trimEnd('/')}/audio/transcriptions")
                .header("Authorization", "Bearer ${config.apiKey}")
                .post(body)
                .build()
            transcribeClient.newCall(req).execute().use { res ->
                if (!res.isSuccessful) {
                    throw IOException("STT HTTP ${res.code}")
                }
                JsonParser.parseString(res.body?.string() ?: "")
                    .asJsonObject?.get("text")?.asString
                    ?: throw IOException("STT: 応答に text が無い")
            }
        }

    /**
     * システムプロンプト + 会話履歴 (question, answer のペア) + 新しい質問で
     * chat/completions を呼び、返答本文を返す。
     *
     * `stream:true` を要求する。サーバーが `text/event-stream` を返せば SSE の差分を
     * 集めて [onDelta] に累積本文を渡す (muse2api など OpenAI 互換サーバー)。
     * JSON で返ってきた場合は従来どおり `choices[0].message.content` を読む。
     */
    suspend fun chat(
        config: AgentConfig,
        systemPrompt: String,
        history: List<Pair<String, String>>,
        question: String,
        onDelta: ((String) -> Unit)? = null,
    ): String = withContext(Dispatchers.IO) {
        val messages = JsonArray()
        messages.add(JsonObject().apply {
            addProperty("role", "system")
            addProperty("content", systemPrompt)
        })
        for ((q, a) in history) {
            messages.add(JsonObject().apply {
                addProperty("role", "user")
                addProperty("content", q)
            })
            messages.add(JsonObject().apply {
                addProperty("role", "assistant")
                addProperty("content", a)
            })
        }
        messages.add(JsonObject().apply {
            addProperty("role", "user")
            addProperty("content", question)
        })
        val body = JsonObject().apply {
            addProperty("model", config.chatModel)
            add("messages", messages)
            // 応答を早く受け取る (対応していないサーバーは JSON で返してくる)。
            addProperty("stream", true)
        }
        val req = Request.Builder()
            .url("${config.baseUrl.trimEnd('/')}/chat/completions")
            .header("Authorization", "Bearer ${config.apiKey}")
            .post(body.toString().toRequestBody("application/json".toMediaType()))
            .build()
        client.newCall(req).execute().use { res ->
            if (!res.isSuccessful) {
                throw IOException("chat HTTP ${res.code}")
            }
            val contentType = res.header("Content-Type") ?: ""
            if (contentType.contains("text/event-stream", ignoreCase = true)) {
                val source = res.body?.source() ?: throw IOException("chat: 応答が空です")
                val parser = SseParser()
                val acc = StringBuilder()
                var done = false
                while (!done) {
                    val line = source.readUtf8Line() ?: break
                    val sse = parser.feedLine(line) ?: continue
                    val data = sse.data.trim()
                    if (data.isEmpty()) continue
                    if (data == "[DONE]") break
                    val o = try {
                        JsonParser.parseString(data) as? JsonObject
                    } catch (e: Exception) {
                        continue
                    } ?: continue
                    val choices = o.getAsJsonArray("choices") ?: continue
                    if (choices.size() == 0) continue
                    val choice = choices.get(0) as? JsonObject ?: continue
                    val delta = choice.getAsJsonObject("delta")?.get("content")?.asString
                    if (!delta.isNullOrEmpty()) {
                        acc.append(delta)
                        onDelta?.invoke(acc.toString())
                    }
                    if (choice.get("finish_reason")?.takeIf { !it.isJsonNull } != null) done = true
                }
                val text = acc.toString()
                if (text.isBlank()) throw IOException("chat: 応答に content が無い")
                return@withContext text
            }
            // 非ストリーム応答 (サーバーが stream を無視した場合)。
            val choices = JsonParser.parseString(res.body?.string() ?: "")
                .asJsonObject?.getAsJsonArray("choices")
            if (choices == null || choices.size() == 0) {
                throw IOException("chat: 応答に content が無い")
            }
            (choices.get(0) as? JsonObject)
                ?.getAsJsonObject("message")?.get("content")?.asString
                ?: throw IOException("chat: 応答に content が無い")
        }
    }
}
