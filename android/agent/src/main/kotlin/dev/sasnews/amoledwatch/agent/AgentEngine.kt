package dev.sasnews.amoledwatch.agent

import dev.sasnews.amoledwatch.protocol.Adpcm

/**
 * 時計の AI 要求を処理するエンジン。
 * - 「話しかける」の録音 (ADP1) → WAV → STT → chat
 * - 定型質問のテキスト → chat
 * 直近 HISTORY_MAX 往復の会話履歴を保持し chat に同梱する。
 * 返り値は常に時計に表示する日本語テキスト (エラーも文字列で返す)。
 */
class AgentEngine(
    private val api: AgentApi = AgentApi(),
    var config: AgentConfig? = null,
) {
    companion object {
        const val SYSTEM_PROMPT =
            "スマートウォッチの小さい画面向けに、日本語で短く答えてください。"
        const val NO_KEY_MESSAGE = "スマホでAI設定をしてください"
        const val HISTORY_MAX = 10

        /** protocol-v1.md agent.reply の text 上限。 */
        const val REPLY_MAX_BYTES = 960
    }

    /** 1往復 (質問→返答)。 */
    data class Turn(val question: String, val answer: String)

    /** 会話履歴。先頭が最古。UI はこの並びをそのまま出す。 */
    val history = ArrayDeque<Turn>()

    /** エラー通知 (デバッグログ用)。 */
    var onError: ((String) -> Unit)? = null

    fun clearHistory() = history.clear()

    /** 履歴を外部ストレージから復元した時に積む。 */
    fun seedHistory(turns: List<Turn>) {
        history.clear()
        history.addAll(turns.takeLast(HISTORY_MAX))
    }

    /** テキストの質問に答える。 */
    suspend fun answerText(question: String): String {
        val cfg = config
        if (cfg == null || !cfg.usable()) return NO_KEY_MESSAGE
        val reply = try {
            api.chat(
                cfg, SYSTEM_PROMPT,
                history.map { it.question to it.answer }, question,
            )
        } catch (e: Exception) {
            onError?.invoke("chat: ${e.message}")
            return "通信エラーです"
        }
        val trimmed = truncateUtf8(reply, REPLY_MAX_BYTES)
        history.addLast(Turn(question, trimmed))
        while (history.size > HISTORY_MAX) history.removeFirst()
        return trimmed
    }

    /** ADP1 音声を STT して答える。 */
    suspend fun answerAudio(adp1: ByteArray): String {
        val wav = Adpcm.adp1ToWav(adp1) ?: return "音声の形式が壊れています"
        val cfg = config
        if (cfg == null || !cfg.usable()) return NO_KEY_MESSAGE
        val text = try {
            api.transcribe(cfg, wav)
        } catch (e: Exception) {
            onError?.invoke("stt: ${e.message}")
            return "音声認識に失敗しました"
        }
        if (text.isBlank()) return "聞き取れませんでした"
        return answerText(text)
    }
}

/** UTF-8 の文字の途中で切らないように maxBytes 以下に切り詰める。 */
internal fun truncateUtf8(s: String, maxBytes: Int): String {
    val b = s.toByteArray(Charsets.UTF_8)
    if (b.size <= maxBytes) return s
    var n = maxBytes
    // 末尾の文字の先頭バイトまで戻って、必要バイト数が収まるか確かめる。
    var head = n
    while (head > 0 && (b[head - 1].toInt() and 0xC0) == 0x80) head--
    if (head > 0) {
        val lead = b[head - 1].toInt() and 0xFF
        val need = when {
            lead < 0x80 -> 1
            lead < 0xE0 -> 2
            lead < 0xF0 -> 3
            lead < 0xF8 -> 4
            else -> 1
        }
        if (n - (head - 1) < need) n = head - 1
    }
    return String(b, 0, n, Charsets.UTF_8)
}
