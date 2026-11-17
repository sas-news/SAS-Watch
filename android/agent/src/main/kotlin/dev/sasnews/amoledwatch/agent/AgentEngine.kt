package dev.sasnews.amoledwatch.agent

import dev.sasnews.amoledwatch.protocol.Adpcm

/**
 * 時計の AI 要求を処理するエンジン。
 * - 「話しかける」の音声 (ADP1) → メモリ内で WAV → STT → 返答
 * - リアルタイム dictation の確定テキスト → 返答 (dictation の音声処理は [DictationSession])
 * - 定型質問のテキスト → 返答
 *
 * 返答経路は [AiRouting] が決める:
 * - Muse トークンあり → Muse (useapi.net) の chat/SSE
 * - なければ OpenAI 互換 chat/completions
 * - どちらも無ければ「スマホでAI設定をしてください」
 *
 * 直近 HISTORY_MAX 往復の会話履歴を保持し OpenAI 経路に同梱する。
 * 返り値は常に時計に表示する日本語テキスト (エラーも文字列で返す)。
 */
class AgentEngine(
    private val api: AgentApi = AgentApi(),
    var config: AgentConfig? = null,
    private val museReply: MuseReplyService = MuseReplyService(),
) {
    companion object {
        const val SYSTEM_PROMPT =
            "スマートウォッチの小さい画面向けに、日本語で短く答えてください。"
        const val NO_KEY_MESSAGE = "スマホでAI設定をしてください"
        const val HISTORY_MAX = 10
        const val COMM_ERROR_MESSAGE = "通信エラーです"

        /** protocol-v1.md agent.reply の text 上限。 */
        const val REPLY_MAX_BYTES = 960
    }

    /** 1往復 (質問→返答)。 */
    data class Turn(val question: String, val answer: String)

    /** 会話履歴。先頭が最古。UI はこの並びをそのまま出す。 */
    val history = ArrayDeque<Turn>()

    /** エラー通知 (デバッグログ用)。 */
    var onError: ((String) -> Unit)? = null

    /** Muse が新規チャットを作ったとき (chatId を保存するため)。 */
    var onChatIdCreated: ((String) -> Unit)? = null

    /** Muse のストリーミング途中経過 (チャット画面の「受信中」表示用)。 */
    var onMuseDelta: ((String) -> Unit)? = null

    /** Muse の承認要求 (v1 は表示のみ)。 */
    var onMuseApproval: ((MuseApproval) -> Unit)? = null

    /** Muse への 1 往復が終わったとき (チャット履歴に出すため)。 */
    var onMuseTurn: ((question: String, answer: String) -> Unit)? = null

    /** OpenAI 互換 chat のストリーミング途中経過 (muse2api 等の SSE 差分)。 */
    var onChatDelta: ((String) -> Unit)? = null

    fun clearHistory() = history.clear()

    /** 履歴を外部ストレージから復元した時に積む。 */
    fun seedHistory(turns: List<Turn>) {
        history.clear()
        history.addAll(turns.takeLast(HISTORY_MAX))
    }

    /** テキストの質問に答える。 */
    suspend fun answerText(question: String): String {
        val cfg = config ?: return NO_KEY_MESSAGE
        return when (AiRouting.chatPlan(cfg)) {
            ChatPlan.MUSE -> answerWithMuse(cfg, question)
            ChatPlan.OPENAI -> answerWithOpenAi(cfg, question)
            ChatPlan.NONE -> NO_KEY_MESSAGE
        }
    }

    /** Muse (useapi.net) に送って返答を返す。チャット画面の履歴にも残す。 */
    private suspend fun answerWithMuse(cfg: AgentConfig, question: String): String {
        val result = try {
            museReply.ask(
                cfg, question,
                onChatId = { id -> onChatIdCreated?.invoke(id) },
                onDelta = { text -> onMuseDelta?.invoke(text) },
                onApproval = { approval -> onMuseApproval?.invoke(approval) },
            )
        } catch (e: MuseException) {
            onError?.invoke("muse: ${e.japanese}")
            return truncateUtf8(e.japanese, REPLY_MAX_BYTES)
        } catch (e: Exception) {
            onError?.invoke("muse: ${e.message}")
            return COMM_ERROR_MESSAGE
        }
        val trimmed = truncateUtf8(result.text, REPLY_MAX_BYTES)
        rememberTurn(question, trimmed)
        if (result.approval == null) onMuseTurn?.invoke(question, trimmed)
        return trimmed
    }

    /** OpenAI 互換 chat/completions で返答する (従来経路)。 */
    private suspend fun answerWithOpenAi(cfg: AgentConfig, question: String): String {
        if (!cfg.usable()) return NO_KEY_MESSAGE
        val reply = try {
            api.chat(
                cfg, SYSTEM_PROMPT,
                history.map { it.question to it.answer }, question,
                onDelta = { text -> onChatDelta?.invoke(text) },
            )
        } catch (e: Exception) {
            onError?.invoke("chat: ${e.message}")
            return COMM_ERROR_MESSAGE
        }
        val trimmed = truncateUtf8(reply, REPLY_MAX_BYTES)
        rememberTurn(question, trimmed)
        return trimmed
    }

    private fun rememberTurn(question: String, answer: String) {
        history.addLast(Turn(question, answer))
        while (history.size > HISTORY_MAX) history.removeFirst()
    }

    /** ADP1 音声を STT して答える (旧 agent_audio 経路。後方互換)。 */
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

    /**
     * dictation の PCM16LE (16kHz mono) を OpenAI 互換 `/audio/transcriptions` で
     * 一括文字起こしする (Meta キーなしのフォールバック, docs/muse-integration.md §5-1)。
     * 失敗・空なら null (理由は [onError])。
     */
    suspend fun transcribePcm(pcm16le: ByteArray): String? {
        val cfg = config
        if (cfg == null || !cfg.usable()) {
            onError?.invoke(NO_KEY_MESSAGE)
            return null
        }
        if (pcm16le.isEmpty()) return null
        return try {
            api.transcribe(cfg, Adpcm.pcm16leToWav(pcm16le)).takeIf { it.isNotBlank() }
        } catch (e: Exception) {
            onError?.invoke("stt: ${e.message}")
            null
        }
    }

    /** WAV を STT するだけ (時計の音声メモ文字起こし, T31)。失敗は null。 */
    suspend fun transcribeOnly(wav: ByteArray): String? {
        val cfg = config
        if (cfg == null || !cfg.usable()) {
            onError?.invoke(NO_KEY_MESSAGE)
            return null
        }
        return try {
            api.transcribe(cfg, wav).takeIf { it.isNotBlank() }
        } catch (e: Exception) {
            onError?.invoke("stt: ${e.message}")
            null
        }
    }
}

/** UTF-8 の文字の途中で切らないように maxBytes 以下に切り詰める。 */
fun truncateUtf8(s: String, maxBytes: Int): String {
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
