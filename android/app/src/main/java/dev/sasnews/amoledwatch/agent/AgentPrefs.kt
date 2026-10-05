package dev.sasnews.amoledwatch.agent

import android.content.Context
import android.content.SharedPreferences
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.json.JSONArray
import org.json.JSONObject

/**
 * AI 設定の保存先。
 * API キーは EncryptedSharedPreferences (鍵は AndroidKeyStore)。
 * 会話履歴は平文の SharedPreferences (秘匿情報ではない)。
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
    )

    fun saveConfig(c: AgentConfig) {
        secure.edit()
            .putString(KEY_BASE_URL, c.baseUrl)
            .putString(KEY_API_KEY, c.apiKey)
            .putString(KEY_CHAT_MODEL, c.chatModel)
            .putString(KEY_STT_MODEL, c.sttModel)
            .apply()
    }

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

    companion object {
        private const val KEY_BASE_URL = "base_url"
        private const val KEY_API_KEY = "api_key"
        private const val KEY_CHAT_MODEL = "chat_model"
        private const val KEY_STT_MODEL = "stt_model"
        private const val KEY_HISTORY = "history"
    }
}
