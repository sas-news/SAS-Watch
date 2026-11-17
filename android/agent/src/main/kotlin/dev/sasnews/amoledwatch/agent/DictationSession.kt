package dev.sasnews.amoledwatch.agent

import java.io.ByteArrayOutputStream
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.Dispatchers

/**
 * 時計の dictation 1 回分 (BULK kind="dictation" の id 単位) のパイプライン。
 *
 * `agent.partial` (部分表示) → 停止 (BULK_END) / 5秒無音 / 切断 → 確定 `agent.partial{final:true}`
 * → 返答 (`agent.reply`) を順に出す。STT は次のどちらか:
 * - [createStt] があれば Vosk / Meta realtime ASR (部分表示あり)。
 * - [batchTranscribe] があれば停止後に一括 STT (部分表示なし)。
 * - どちらも無ければ「スマホでAI設定をしてください」(docs/muse-integration.md §5-3)。
 *
 * コールバックは suspend で、時計への送信や Muse 呼び出しをそのまま渡せる。
 */
class DictationSession(
    val id: Int,
    private val createStt: ((onPartial: (String) -> Unit, onError: (String) -> Unit) -> SttEngine)? = null,
    private val batchTranscribe: (suspend (ByteArray) -> String?)? = null,
    private val ask: suspend (String) -> String = { AgentEngine.NO_KEY_MESSAGE },
    private val onPartial: suspend (text: String, final: Boolean) -> Unit = { _, _ -> },
    private val onReply: suspend (text: String) -> Unit = {},
    private val scope: CoroutineScope,
    private val silenceMs: Long = SILENCE_TIMEOUT_MS,
    private val finalTimeoutMs: Long = ASR_FINAL_TIMEOUT_MS,
    /** セッションが畳まれた直後 (BLE の接続優先度を戻す等) に 1 回だけ呼ばれる。 */
    private val onFinished: () -> Unit = {},
    /** STT も返答も設定されていないときに時計へ出す文言。 */
    private val notReadyMessage: String = AgentEngine.NO_KEY_MESSAGE,
) {
    companion object {
        /** docs/protocol-v1.md: 5 秒 audio が来なくなったら自動確定。 */
        const val SILENCE_TIMEOUT_MS = 5_000L
        const val NO_HEARING_MESSAGE = "聞き取れませんでした"
        const val STT_FAILED_MESSAGE = "音声認識に失敗しました"
        const val COMM_ERROR = "通信エラーです"

        /**
         * 一括 STT フォールバック時にメモリへ貯める PCM の上限 (16MB ≒ 16kHz mono で約8分)。
         * 3 分の発話 (≒5.8MB) は余裕で収まる。超えた分は捨てて [Stats.batchTruncated] を立てる。
         * 既定の Vosk / Meta realtime 経路はキューが一定量で、ここは使わない。
         */
        const val MAX_BATCH_PCM_BYTES = 16L * 1024 * 1024
    }

    /** スループットのログ用。 */
    data class Stats(
        val pcmBytes: Long,
        val durationMs: Long,
        val droppedBytes: Long,
        /** 一括 STT 用バッファが上限に達して末尾を捨てたか (通常の Vosk 経路では false)。 */
        val batchTruncated: Boolean = false,
    )

    @Volatile private var stt: SttEngine? = null
    @Volatile private var sttError: String? = null
    @Volatile private var lastPartial = ""
    private val batch = ByteArrayOutputStream()
    private val started = AtomicBoolean(false)
    private val finished = AtomicBoolean(false)
    private var silenceJob: Job? = null
    private var startedAtMs = 0L
    private var pcmBytes = 0L
    private var batchTruncated = false

    fun start() {
        if (!started.compareAndSet(false, true)) return
        startedAtMs = System.currentTimeMillis()
        val factory = createStt ?: run {
            restartSilence()
            return
        }
        val created = try {
            factory(
                { text ->
                    lastPartial = text
                    scope.launch { onPartial(text, false) }
                },
                { message -> sttError = message },
            )
        } catch (e: Exception) {
            // Vosk モデル未準備など: 時計に具体的な日本語を返す。
            sttError = e.message ?: notReadyMessage
            restartSilence()
            return
        }
        stt = created
        created.start()
        restartSilence()
    }

    /** PCM16LE 16kHz mono を 1 チャンク受けた。 */
    fun onPcm(pcm: ByteArray) {
        if (finished.get()) return
        start()
        if (pcm.isEmpty()) return
        pcmBytes += pcm.size
        val r = stt
        if (r != null) {
            r.feed(pcm)
        } else {
            // 一括 STT フォールバック: ディスクには書かず、上限付きのメモリに貯める。
            if (batch.size().toLong() + pcm.size <= MAX_BATCH_PCM_BYTES) {
                batch.write(pcm)
            } else {
                batchTruncated = true
            }
        }
        restartSilence()
    }

    /**
     * 音声入力の停止 (BULK_END) / 無音タイムアウトで確定する。
     * 何度呼んでも 1 回だけ実行される。
     */
    suspend fun finish() {
        if (!finished.compareAndSet(false, true)) return
        silenceJob?.cancel()
        silenceJob = null
        onFinished()

        val realtime = stt
        val text: String? = when {
            realtime != null -> {
                val final = realtime.finish(finalTimeoutMs)
                final?.trim()?.takeIf { it.isNotEmpty() }
                    ?: lastPartial.trim().takeIf { it.isNotEmpty() }
            }
            batchTranscribe != null -> withContext(Dispatchers.IO) {
                batchTranscribe(batch.toByteArray())?.trim()?.takeIf { it.isNotEmpty() }
            }
            else -> null
        }
        realtime?.cancel()

        if (text != null) {
            onPartial(text, true)
        }
        val reply = when {
            text != null -> try {
                ask(text)
            } catch (e: Exception) {
                COMM_ERROR
            }
            realtime != null -> sttError ?: NO_HEARING_MESSAGE
            batchTranscribe != null -> sttError ?: STT_FAILED_MESSAGE
            else -> sttError ?: notReadyMessage
        }
        onReply(reply)
    }

    /** 切断などでセッションを畳む (返答はしない)。 */
    fun cancel() {
        if (!finished.compareAndSet(false, true)) return
        silenceJob?.cancel()
        silenceJob = null
        stt?.cancel()
        onFinished()
    }

    fun stats(): Stats = Stats(
        pcmBytes = pcmBytes,
        durationMs = if (startedAtMs == 0L) 0 else System.currentTimeMillis() - startedAtMs,
        droppedBytes = stt?.droppedBytes ?: 0L,
        batchTruncated = batchTruncated,
    )

    private fun restartSilence() {
        silenceJob?.cancel()
        silenceJob = scope.launch {
            delay(silenceMs)
            finish()
        }
    }
}
