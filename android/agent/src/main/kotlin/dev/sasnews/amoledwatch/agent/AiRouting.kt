package dev.sasnews.amoledwatch.agent

import dev.sasnews.amoledwatch.protocol.ActionSpec

/**
 * STT / 返答の経路選択。
 *
 * **既定は無料構成**: 文字起こしはスマホ内の Vosk (オフライン)、返答は自前サーバーの
 * muse2api (OpenAI 互換)。有料の Meta realtime ASR / useapi.net Muse は
 * 「上級者向け」で明示したときだけ使う (新規の課金依存は足さない)。
 *
 * 1. Vosk モデル未準備 → 「設定で音声モデルをダウンロードしてください」
 * 2. Meta realtime ASR は上級者向け (Meta API キーがある場合のみ)
 * 3. OpenAI 互換 STT はフォールバック (部分表示なし)
 * 4. 返答は OpenAI 互換 (muse2api / 従来設定) が主経路。
 */
object SttModes {
    /** Meta realtime ASR があれば使い、なければ OpenAI 互換にフォールバック。 */
    const val AUTO = "auto"

    /** Muse Voice Transcribe (Meta realtime) を優先。 */
    const val MUSE_REALTIME = "muse_realtime"

    /** OpenAI 互換 /audio/transcriptions のみ (部分表示なし)。 */
    const val OPENAI = "openai"

    /** スマホ内 Vosk (オフライン・無料)。 */
    const val VOSK = "vosk"

    /** STT しない。 */
    const val NONE = "none"

    val ALL: List<ActionSpec> = listOf(
        ActionSpec(AUTO, "自動（スマホ内 Vosk）"),
        ActionSpec(VOSK, "スマホ内 Vosk（オフライン）"),
        ActionSpec(OPENAI, "OpenAI互換（一括）"),
        ActionSpec(MUSE_REALTIME, "Muse realtime（Meta・有料/上級者向け）"),
        ActionSpec(NONE, "使わない"),
    )

    fun label(name: String): String = ALL.find { it.name == name }?.labelJa ?: name
}

/** STT (音声→テキスト) の経路。 */
enum class SttPlan {
    /** スマホ内の Vosk でリアルタイム認識 (無料・既定)。 */
    VOSK,

    /** Vosk モデルが未ダウンロード (設定画面でダウンロードを促す)。 */
    VOSK_NOT_READY,

    /** Meta realtime ASR に PCM をストリーミング (有料・上級者向け)。 */
    MUSE_REALTIME,

    /** 音声を貯めてから OpenAI 互換 STT (部分表示なし)。 */
    OPENAI_BATCH,

    /** STT しない。 */
    NONE,
}

/** 返答 (テキスト→応答) の経路。 */
enum class ChatPlan {
    /** Muse (useapi.net) の chat / SSE。 */
    MUSE,

    /** 従来の OpenAI 互換 chat/completions。 */
    OPENAI,

    /** 返答経路なし。 */
    NONE,
}

object AiRouting {
    /** Vosk モデルが未ダウンロードのときに時計へ出す文言。 */
    const val VOSK_MODEL_MISSING = "設定で音声モデルをダウンロードしてください"

    /**
     * 音声入力の STT 経路を決める。
     * @param voskReady スマホ内 Vosk モデルが展開済みか (既定 false)。
     */
    fun sttPlan(config: AgentConfig?, voskReady: Boolean = false): SttPlan {
        val c = config ?: return SttPlan.NONE
        return when (c.sttMode) {
            SttModes.NONE -> SttPlan.NONE
            SttModes.VOSK -> if (voskReady) SttPlan.VOSK else SttPlan.VOSK_NOT_READY
            SttModes.OPENAI -> if (c.usable()) SttPlan.OPENAI_BATCH else voskOrNotReady(voskReady)
            SttModes.MUSE_REALTIME ->
                if (c.metaAsrUsable()) SttPlan.MUSE_REALTIME
                // Meta キーが無ければ無料の既定 (Vosk) に戻す。
                else voskOrNotReady(voskReady)
            else /* auto */ -> voskOrNotReady(voskReady)
        }
    }

    private fun voskOrNotReady(voskReady: Boolean): SttPlan =
        if (voskReady) SttPlan.VOSK else SttPlan.VOSK_NOT_READY

    /** 返答の経路を決める。 */
    fun chatPlan(config: AgentConfig?): ChatPlan {
        val c = config ?: return ChatPlan.NONE
        return when {
            c.museUsable() -> ChatPlan.MUSE
            c.usable() -> ChatPlan.OPENAI
            else -> ChatPlan.NONE
        }
    }

    /** 何も設定されていなければ true (時計に「スマホでAI設定をしてください」を出す)。 */
    fun nothingConfigured(config: AgentConfig?, voskReady: Boolean = false): Boolean =
        sttPlan(config, voskReady) == SttPlan.NONE && chatPlan(config) == ChatPlan.NONE

    /** 日本語の経路名 (デバッグ/ログ用)。 */
    fun describe(config: AgentConfig?, voskReady: Boolean = false): String =
        "stt=${sttPlan(config, voskReady)} chat=${chatPlan(config)}"
}
