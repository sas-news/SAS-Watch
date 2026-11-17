package dev.sasnews.amoledwatch.agent

/**
 * Server-Sent Events (text/event-stream) の最小パーサ (純 Kotlin)。
 * useapi.net Muse の応答ストリーム (docs/muse-integration.md §3.2) を読むのに使う。
 *
 * 仕様: イベントは空行で区切る。`data:` は複数行あれば改行で連結。
 * `:` で始まる行はコメント。`event:` / `id:` / `retry:` を解釈する。
 * 1 行ずつ [feedLine] に渡し、イベント境界で [SseEvent] が返る。
 */
data class SseEvent(
    val id: String?,
    val event: String?,
    val data: String,
)

class SseParser {
    private var id: String? = null
    private var event: String? = null
    private val data = StringBuilder()
    private var hasData = false

    /** 1 行を渡す。空行 (イベント境界) でイベントが完成したら返す。 */
    fun feedLine(rawLine: String): SseEvent? {
        // 行末の CR はサーバによって付く/付かないがあるので落とす。
        val line = rawLine.removeSuffix("\r")
        if (line.isEmpty()) return dispatch()
        if (line.startsWith(":")) return null

        val colon = line.indexOf(':')
        val field: String
        var value: String
        if (colon < 0) {
            field = line
            value = ""
        } else {
            field = line.substring(0, colon)
            value = line.substring(colon + 1)
            if (value.startsWith(" ")) value = value.substring(1)
        }
        when (field) {
            "data" -> {
                if (hasData) data.append('\n')
                data.append(value)
                hasData = true
            }
            "event" -> event = value
            "id" -> id = value.ifEmpty { null }
            // "retry" は再接続待ち時間のヒント。使わない。
        }
        return null
    }

    /** ストリーム終端で残りを確定する (最後が空行で終わっていない場合)。 */
    fun finish(): SseEvent? = dispatch()

    /** テスト用: テキスト全体をまとめて流す。 */
    fun feedAll(text: String): List<SseEvent> {
        val out = ArrayList<SseEvent>()
        for (line in text.split('\n')) {
            feedLine(line)?.let { out += it }
        }
        finish()?.let { out += it }
        return out
    }

    private fun dispatch(): SseEvent? {
        if (!hasData) {
            // data の無いイベント (id だけ等) は配らない。フィールドはリセットする。
            id = null
            event = null
            return null
        }
        val e = SseEvent(id, event, data.toString())
        id = null
        event = null
        data.setLength(0)
        hasData = false
        return e
    }
}
