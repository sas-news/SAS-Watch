package dev.sasnews.amoledwatch.agent

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.json.JSONArray
import org.json.JSONObject

/**
 * AI 設定の保存先。
 * API キー/トークン (OpenAI 互換 / useapi.net / Meta) は
 * EncryptedSharedPreferences (鍵は AndroidKeyStore) に入れる。
 * 会話履歴と Muse チャットの表示履歴は平文の SharedPreferences (秘匿情報ではない)。
 */
class AgentPrefs(context: Context) {

    private val secure: SharedPreferences = try {
        EncryptedSharedPreferences.create(
            context, "agent_secure",
            MasterKey.Builder(context)
                .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
                .build(),
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
        )
    } catch (e: Exception) {
        // AndroidKeyStore が不安定な端末向けフォールバック
        context.getSharedPreferences("agent_secure_fallback", Context.MODE_PRIVATE)
    }

    private val plain = context.getSharedPreferences("agent", Context.MODE_PRIVATE)

    fun config(): AgentConfig = AgentConfig(
        baseUrl = secure.getString(KEY_BASE_URL, AgentConfig().baseUrl)
            ?: AgentConfig().baseUrl,
        apiKey = secure.getString(KEY_API_KEY, "") ?: "",
        chatModel = secure.getString(KEY_CHAT_MODEL, AgentConfig().chatModel)
            ?: AgentConfig().chatModel,
        sttModel = secure.getString(KEY_STT_MODEL, AgentConfig().sttModel)
            ?: AgentConfig().sttModel,
        museEnabled = secure.getBoolean(KEY_MUSE_ENABLED, false),
        museToken = secure.getString(KEY_MUSE_TOKEN, "") ?: "",
        museChatId = secure.getString(KEY_MUSE_CHAT_ID, "") ?: "",
        museEmail = secure.getString(KEY_MUSE_EMAIL, "") ?: "",
        metaApiKey = secure.getString(KEY_META_API_KEY, "") ?: "",
        sttMode = secure.getString(KEY_STT_MODE, SttModes.AUTO) ?: SttModes.AUTO,
        languageBias = decodeList(
            secure.getString(KEY_LANGUAGE_BIAS, "Japanese") ?: "Japanese",
            default = listOf("Japanese"),
        ),
        asrKeywords = decodeList(secure.getString(KEY_ASR_KEYWORDS, "") ?: ""),
    )

    fun saveConfig(c: AgentConfig) {
        secure.edit()
            .putString(KEY_BASE_URL, c.baseUrl)
            .putString(KEY_API_KEY, c.apiKey)
            .putString(KEY_CHAT_MODEL, c.chatModel)
            .putString(KEY_STT_MODEL, c.sttModel)
            .putBoolean(KEY_MUSE_ENABLED, c.museEnabled)
            .putString(KEY_MUSE_TOKEN, c.museToken)
            .putString(KEY_MUSE_CHAT_ID, c.museChatId)
            .putString(KEY_MUSE_EMAIL, c.museEmail)
            .putString(KEY_META_API_KEY, c.metaApiKey)
            .putString(KEY_STT_MODE, c.sttMode)
            .putString(KEY_LANGUAGE_BIAS, encodeList(c.languageBias))
            .putString(KEY_ASR_KEYWORDS, encodeList(c.asrKeywords))
            .apply()
    }

    /**
     * Muse の chatId だけを保存する (チャット新規作成時に自動保存)。
     * 保存済み設定の他の項目は変えない。
     */
    fun saveMuseChatId(chatId: String) {
        secure.edit().putString(KEY_MUSE_CHAT_ID, chatId).apply()
    }

    /** 有料の上級者向け設定 (useapi.net Muse / Meta realtime ASR) を画面に出すか。 */
    fun advancedAi(): Boolean = plain.getBoolean(KEY_ADVANCED_AI, false)

    fun setAdvancedAi(enabled: Boolean) {
        plain.edit().putBoolean(KEY_ADVANCED_AI, enabled).apply()
    }

    // ---------------- 時計 AI の会話履歴 ----------------

    fun loadHistory(): List<AgentEngine.Turn> {
        val raw = plain.getString(KEY_HISTORY, "[]") ?: "[]"
        return try {
            val arr = JSONArray(raw)
            (0 until arr.length()).mapNotNull { i ->
                val o = arr.optJSONObject(i) ?: return@mapNotNull null
                AgentEngine.Turn(o.optString("q"), o.optString("a"))
            }
        } catch (e: Exception) {
            emptyList()
        }
    }

    fun saveHistory(turns: List<AgentEngine.Turn>) {
        val arr = JSONArray()
        for (t in turns) {
            arr.put(JSONObject().put("q", t.question).put("a", t.answer))
        }
        plain.edit().putString(KEY_HISTORY, arr.toString()).apply()
    }

    fun clearHistory() = plain.edit().remove(KEY_HISTORY).apply()

    // ---------------- Muse チャット画面の履歴 (アプリ側) ----------------

    /** 表示用の 1 メッセージ。role は "user" / "assistant" / "system"。 */
    data class MuseLine(val role: String, val text: String, val epochMs: Long = 0)

    fun loadMuseMessages(chatId: String): List<MuseLine> {
        val raw = plain.getString(museKey(chatId), "[]") ?: "[]"
        return try {
            val arr = JSONArray(raw)
            (0 until arr.length()).mapNotNull { i ->
                val o = arr.optJSONObject(i) ?: return@mapNotNull null
                MuseLine(
                    o.optString("role"),
                    o.optString("text"),
                    o.optLong("t"),
                )
            }
        } catch (e: Exception) {
            emptyList()
        }
    }

    fun saveMuseMessages(chatId: String, lines: List<MuseLine>) {
        val arr = JSONArray()
        for (l in lines.takeLast(MUSE_HISTORY_MAX)) {
            arr.put(JSONObject().put("role", l.role).put("text", l.text).put("t", l.epochMs))
        }
        plain.edit().putString(museKey(chatId), arr.toString()).apply()
    }

    fun clearMuseMessages(chatId: String) = plain.edit().remove(museKey(chatId)).apply()

    private fun museKey(chatId: String) = "muse_${chatId.hashCode()}"

    companion object {
        private const val KEY_BASE_URL = "base_url"
        private const val KEY_API_KEY = "api_key"
        private const val KEY_CHAT_MODEL = "chat_model"
        private const val KEY_STT_MODEL = "stt_model"
        private const val KEY_MUSE_ENABLED = "muse_enabled"
        private const val KEY_MUSE_TOKEN = "muse_token"
        private const val KEY_MUSE_CHAT_ID = "muse_chat_id"
        private const val KEY_MUSE_EMAIL = "muse_email"
        private const val KEY_META_API_KEY = "meta_api_key"
        private const val KEY_STT_MODE = "stt_mode"
        private const val KEY_LANGUAGE_BIAS = "language_bias"
        private const val KEY_ASR_KEYWORDS = "asr_keywords"
        private const val KEY_HISTORY = "history"
        private const val KEY_ADVANCED_AI = "advanced_ai"

        /** Muse チャット画面に残す最大行数。 */
        const val MUSE_HISTORY_MAX = 200

        /** カンマ/読点区切りの設定値を配列に。 */
        fun decodeList(raw: String, default: List<String> = emptyList()): List<String> {
            if (raw.isBlank()) return default
            return raw.split(',', '、', '\n')
                .map { it.trim() }
                .filter { it.isNotEmpty() }
                .ifEmpty { default }
        }

        /** 配列をカンマ区切りに。 */
        fun encodeList(values: List<String>): String =
            values.map { it.trim() }.filter { it.isNotEmpty() }.joinToString(",")
    }
}
