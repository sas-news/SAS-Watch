package dev.sasnews.amoledwatch.agent

import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.vosk.Recognizer

/**
 * スマホ内 Vosk で PCM16LE 16kHz mono をリアルタイム認識する [SttEngine]。
 *
 * - [feed] はキューに積むだけ (BLE 通知スレッドを止めない)。ワーカーが順に
 *   `Recognizer.acceptWaveForm` に流し、`partialResult()` を `agent.partial` 用に返す。
 * - 発話の区切り (`acceptWaveForm` が true) では `result()` の文を積んで
 *   累積の partial を出す。
 * - [finish] で残りを drain して `finalResult()` を確定テキストにする
 *   (無音/音声入力の停止で [DictationSession] から呼ばれる)。
 *
 * モデル自体は [VoskModelStore] が管理し、ここはセッションごとの Recognizer を持つ。
 */
class VoskSttEngine(
    private val recognizer: Recognizer,
    private val onPartial: (String) -> Unit = {},
    private val onError: (String) -> Unit = {},
) : SttEngine {

    companion object {
        private const val TAG = "VoskSttEngine"

        /** キューに許す最大音声 (約 2 秒)。溢れたら古い方から捨てる。 */
        const val MAX_QUEUE_BYTES = 64_000

        const val RECOGNIZE_FAILED = "音声認識に失敗しました"

        /**
         * [cancel] で認識スレッドの後始末を待つ上限。
         * **発話の長さ制限ではない** (確定は BULK_END / 5秒無音 / 切断のみ)。
         */
        const val CANCEL_CLOSE_TIMEOUT_MS = 3_000L
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val lock = Any()
    private val queue = ArrayDeque<ByteArray>()
    private var queuedBytes = 0

    private val done = CompletableDeferred<Unit>()
    private val closed = AtomicBoolean(false)

    @Volatile private var worker: Job? = null
    @Volatile private var finishing = false
    @Volatile private var finalText: String? = null
    @Volatile private var failure: String? = null
    @Volatile private var partial = ""
    @Volatile private var prefix = ""
    @Volatile private var dropped = 0L

    override val partialText: String get() = partial
    override val errorText: String? get() = failure
    override val droppedBytes: Long get() = dropped

    @Synchronized
    override fun start() {
        if (worker != null) return
        worker = scope.launch { run() }
    }

    override fun feed(pcm: ByteArray) {
        if (pcm.isEmpty() || finishing) return
        start()
        synchronized(lock) {
            queue.addLast(pcm)
            queuedBytes += pcm.size
            while (queuedBytes > MAX_QUEUE_BYTES && queue.isNotEmpty()) {
                val old = queue.removeFirst()
                queuedBytes -= old.size
                dropped += old.size
            }
        }
    }

    override suspend fun finish(timeoutMs: Long): String? {
        finishing = true
        val finished = withTimeoutOrNull(timeoutMs) { done.await() } != null
        if (!finished) {
            Log.w(TAG, "finish timeout; using partial result")
        }
        scope.cancel()
        return finalText ?: prefix.ifBlank { partial }.ifBlank { null }
    }

    override fun cancel() {
        finishing = true
        synchronized(lock) {
            queue.clear()
            queuedBytes = 0
        }
        if (worker == null) {
            closeRecognizer()
            scope.cancel()
            return
        }
        scope.launch {
            withTimeoutOrNull(CANCEL_CLOSE_TIMEOUT_MS) { done.await() }
            scope.cancel()
        }
    }

    // ---------------- ワーカー ----------------

    private suspend fun run() {
        try {
            while (scope.isActive) {
                val chunk = poll()
                if (chunk == null) {
                    if (finishing) break
                    delay(5)
                    continue
                }
                process(chunk)
            }
            try {
                val text = VoskResults.text(recognizer.finalResult)
                finalText = combine(text)
            } catch (e: Exception) {
                Log.w(TAG, "finalResult failed: ${e.message}")
                finalText = combine("")
            }
        } finally {
            closeRecognizer()
            done.complete(Unit)
        }
    }

    private fun process(chunk: ByteArray) {
        try {
            if (recognizer.acceptWaveForm(chunk, chunk.size)) {
                // 発話の区切り: 文を積んで次の発話へ。
                val text = VoskResults.text(recognizer.result)
                if (text.isNotBlank()) prefix = combine(text)
                publish(prefix)
            } else {
                val p = VoskResults.partial(recognizer.partialResult)
                if (p.isNotBlank()) publish(combine(p))
            }
        } catch (e: Exception) {
            Log.w(TAG, "recognize failed: ${e.message}")
            if (failure == null) {
                failure = RECOGNIZE_FAILED
                onError(failure!!)
            }
        }
    }

    /** 同じ文字列を連続で送らない (BLE の agent.partial を増やさない)。 */
    private fun publish(text: String) {
        if (text.isBlank() || text == partial) return
        partial = text
        onPartial(text)
    }

    /** 確定済みの文 [prefix] と今の発話 [tail] を連結する。 */
    private fun combine(tail: String): String {
        val a = prefix.trim()
        val b = tail.trim()
        return when {
            a.isEmpty() -> b
            b.isEmpty() -> a
            else -> "$a $b"
        }
    }

    private fun poll(): ByteArray? = synchronized(lock) {
        val a = queue.removeFirstOrNull() ?: return null
        queuedBytes -= a.size
        a
    }

    private fun closeRecognizer() {
        if (!closed.compareAndSet(false, true)) return
        try {
            recognizer.close()
        } catch (e: Exception) {
            Log.w(TAG, "recognizer close failed: ${e.message}")
        }
    }
}
