package dev.sasnews.amoledwatch.agent

import android.util.Log
import dev.sasnews.amoledwatch.connection.WatchLink
import dev.sasnews.amoledwatch.protocol.IncomingBulk
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.protocol.Res
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * 時計の AI 要求 → OpenAI 互換 API → `agent.reply` を返す橋渡し。
 * - EVT agent.request (定型質問) → `onRequest`
 * - BULK kind="agent_audio" (話しかけた録音, ADP1) → `onBulk`
 * WatchLinkManager が接続ごとにここへ流す。応答は直列化して順に返す。
 */
class AgentBridge(
    private val prefs: AgentPrefs,
    private val scope: CoroutineScope =
        CoroutineScope(SupervisorJob() + Dispatchers.Default),
) {
    companion object {
        private const val TAG = "AgentBridge"
    }

    private val engine = AgentEngine()

    private val _config = MutableStateFlow(prefs.config())
    val config: StateFlow<AgentConfig> = _config

    private val _history = MutableStateFlow<List<AgentEngine.Turn>>(emptyList())
    val history: StateFlow<List<AgentEngine.Turn>> = _history

    /** 処理中の要求数 (デバッグ表示用)。 */
    private val _busy = MutableStateFlow(0)
    val busy: StateFlow<Int> = _busy

    /** 応答の直列化 (履歴順を保つ)。 */
    private val mutex = Mutex()

    var onError: ((String) -> Unit)? = null

    init {
        engine.seedHistory(prefs.loadHistory())
        engine.onError = { msg -> onError?.invoke(msg) }
        engine.config = _config.value
        _history.value = engine.history.toList()
    }

    fun updateConfig(c: AgentConfig) {
        prefs.saveConfig(c)
        _config.value = c
        engine.config = c
    }

    fun clearHistory() {
        scope.launch {
            mutex.withLock {
                engine.clearHistory()
                prefs.clearHistory()
                _history.value = emptyList()
            }
        }
    }

    /** EVT agent.request (定型質問のテキスト)。 */
    fun onRequest(link: WatchLink, id: Int, text: String) {
        scope.launch {
            reply(link, id) { engine.answerText(text) }
        }
    }

    /** BULK kind="agent_audio" (録音 ADP1)。他の kind はここでは処理しない。 */
    fun onBulk(link: WatchLink, t: IncomingBulk) {
        if (t.kind != "agent_audio") return
        scope.launch {
            reply(link, t.id) { engine.answerAudio(t.bytes) }
        }
    }

    private suspend fun reply(link: WatchLink, id: Int, ask: suspend () -> String) {
        _busy.value++
        val text = try {
            mutex.withLock { ask() }
        } finally {
            _busy.value--
        }
        _history.value = engine.history.toList()
        prefs.saveHistory(engine.history.toList())
        val res = link.request(Req.AgentReply(id, text))
        if (res is Res.Err) {
            Log.w(TAG, "agent.reply failed: ${res.code} ${res.message}")
            onError?.invoke("agent.reply: ${res.code}")
        }
    }
}
