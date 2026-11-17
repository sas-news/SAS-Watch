package dev.sasnews.amoledwatch.agent

import com.google.gson.JsonObject
import com.google.gson.JsonParser

/**
 * Vosk の結果 JSON を読むための最小ヘルパ (純 Kotlin)。
 *
 * Vosk Android の `Recognizer` は
 * - `partialResult()` → `{"partial":"..."} `
 * - `result()` / `finalResult()` → `{"text":"..."} `
 * のような JSON 文字列を返す。ネイティブライブラリを読まずにテストできるよう
 * パースだけをここに分離している。
 */
object VoskResults {
    fun partial(json: String): String = field(json, "partial")

    fun text(json: String): String = field(json, "text")

    private fun field(json: String, name: String): String {
        if (json.isBlank()) return ""
        val o = try {
            JsonParser.parseString(json) as? JsonObject
        } catch (e: Exception) {
            null
        } ?: return ""
        return o.get(name)?.takeIf { !it.isJsonNull }?.asString ?: ""
    }
}
