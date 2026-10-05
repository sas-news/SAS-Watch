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

/** OpenAI 互換 API の接続設定 (設定画面「AI設定」で編集)。 */
data class AgentConfig(
    /** 例: "https://api.openai.com/v1"。ローカルの LLM サーバでも可。 */
    val baseUrl: String = "https://api.openai.com/v1",
    val apiKey: String = "",
    val chatModel: String = "gpt-4o-mini",
    val sttModel: String = "whisper-1",
) {
    /** API キーと Base URL が揃っているか。 */
    fun usable(): Boolean = baseUrl.isNotBlank() && apiKey.isNotBlank()
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

    /** WAV を送って書き起こしテキストを返す。 */
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
            client.newCall(req).execute().use { res ->
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
     */
    suspend fun chat(
        config: AgentConfig,
        systemPrompt: String,
        history: List<Pair<String, String>>,
        question: String,
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
            JsonParser.parseString(res.body?.string() ?: "")
                .asJsonObject?.getAsJsonArray("choices")
                ?.get(0)?.asJsonObject
                ?.getAsJsonObject("message")?.get("content")?.asString
                ?: throw IOException("chat: 応答に content が無い")
        }
    }
}
