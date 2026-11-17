package dev.sasnews.amoledwatch.agent

import android.util.Log
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.connection.WatchLink
import dev.sasnews.amoledwatch.protocol.IncomingBulk
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.protocol.Res
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

/**
 * 時計の AI 要求 → STT / Muse (または OpenAI 互換) → `agent.partial` / `agent.reply`
 * を返す橋渡し。
 *
 * - EVT agent.request (定型質問) → Muse / OpenAI 互換 chat
 * - BULK kind="dictation" → ADPCM は WatchLink 側で PCM にデコード済み。
 *   [DictationSession] が Meta realtime ASR (または一括 STT) → 部分表示 → 返答を出す。
 * - BULK kind="agent_audio" (旧経路) → 従来の一括 STT (後方互換)
 * - Muse チャット画面: 会話一覧 / 履歴 / テキスト送信 / ストリーミング表示
 *
 * 秘密値は AgentPrefs (EncryptedSharedPreferences) に保存する。
 */
class AgentBridge(
    private val prefs: AgentPrefs,
    private val scope: CoroutineScope =
        CoroutineScope(SupervisorJob() + Dispatchers.Default),
) {
    companion object {
        private const val TAG = "AgentBridge"

        /** 切断中に出せなかった返答を保持する上限 (再接続時に順に送る)。 */
        private const val PENDING_REPLY_MAX = 8

        /** 切断中の返答を自動再送するために接続復帰を待つ上限。 */
        private const val PENDING_REPLY_WAIT_MS = 60_000L
    }

    private val engine = AgentEngine()

    private val _config = MutableStateFlow(prefs.config())
    val config: StateFlow<AgentConfig> = _config

    private val _history = MutableStateFlow<List<AgentEngine.Turn>>(emptyList())
    val history: StateFlow<List<AgentEngine.Turn>> = _history

    /** 処理中の要求数 (デバッグ表示用)。 */
    private val _busy = MutableStateFlow(0)
    val busy: StateFlow<Int> = _busy

    /** 有料の上級者向け設定 (useapi.net Muse / Meta ASR) を画面に出すか。 */
    private val _advancedAi = MutableStateFlow(prefs.advancedAi())
    val advancedAi: StateFlow<Boolean> = _advancedAi

    /** 応答の直列化 (履歴順を保つ)。 */
    private val mutex = Mutex()

    var onError: ((String) -> Unit)? = null

    /** スマホ内 Vosk モデルが展開済みか (WatchApp が設定)。 */
    var voskReady: () -> Boolean = { false }

    /** スマホ内 Vosk の [SttEngine] を作る (WatchApp が VoskModelStore で設定)。 */
    var createVoskStt: ((onPartial: (String) -> Unit, onError: (String) -> Unit) -> SttEngine?)? = null

    // ---------------- Muse チャット画面 ----------------

    /** 会話一覧 (GET /chats)。 */
    private val _museChats = MutableStateFlow<List<MuseChatSummary>>(emptyList())
    val museChats: StateFlow<List<MuseChatSummary>> = _museChats

    /** 選択中の chatId (空 = 新規会話)。 */
    private val _museChatId = MutableStateFlow(prefs.config().museChatId)
    val museChatId: StateFlow<String> = _museChatId

    /** 表示中のメッセージ。 */
    private val _museMessages = MutableStateFlow<List<MuseUiMessage>>(emptyList())
    val museMessages: StateFlow<List<MuseUiMessage>> = _museMessages

    /** 一覧の読み込み中。 */
    private val _museLoading = MutableStateFlow(false)
    val museLoading: StateFlow<Boolean> = _museLoading

    /** 送信中/ストリーミング受信中。 */
    private val _museBusy = MutableStateFlow(false)
    val museBusy: StateFlow<Boolean> = _museBusy

    /** 承認要求 (スマホの Muse アプリで承認する)。 */
    private val _museApproval = MutableStateFlow<MuseApproval?>(null)
    val museApproval: StateFlow<MuseApproval?> = _museApproval

    /** チャット画面用の一言メッセージ (エラー等)。 */
    private val _museNotice = MutableStateFlow<String?>(null)
    val museNotice: StateFlow<String?> = _museNotice

    fun consumeMuseNotice() {
        _museNotice.value = null
    }

    /** 上級者向け設定 (有料の Muse / Meta ASR) の表示切り替え。 */
    fun setAdvancedAi(enabled: Boolean) {
        prefs.setAdvancedAi(enabled)
        _advancedAi.value = enabled
    }

    private val museApi = MuseApi()
    private val museChatService = MuseReplyService(museApi)

    /** 表示用メッセージ。streaming=true の行は差分更新中。 */
    data class MuseUiMessage(
        val role: String,
        val text: String,
        val streaming: Boolean = false,
        val fromWatch: Boolean = false,
    )

    private data class PendingReply(val link: WatchLink, val id: Int, val text: String)

    private val pendingReplies = ArrayList<PendingReply>()

    /** 進行中の dictation セッション (id = BULK 転送 id = 時計の要求 id)。 */
    private val sessions = ConcurrentHashMap<Int, DictationSession>()

    init {
        engine.seedHistory(prefs.loadHistory())
        engine.onError = { msg -> onError?.invoke(msg) }
        engine.config = _config.value
        engine.onChatIdCreated = { chatId -> onMuseChatIdCreated(chatId) }
        engine.onMuseApproval = { approval ->
            _museApproval.value = approval
            appendSystemLine(MuseApi.APPROVAL_MESSAGE)
        }
        engine.onMuseTurn = { question, answer -> appendWatchTurn(question, answer) }
        _history.value = engine.history.toList()
        _museMessages.value = loadLocal(_museChatId.value)
    }

    fun updateConfig(c: AgentConfig) {
        prefs.saveConfig(c)
        _config.value = c
        engine.config = c
        if (_museChatId.value.isBlank() && c.museChatId.isNotBlank()) {
            _museChatId.value = c.museChatId
            _museMessages.value = loadLocal(c.museChatId)
        }
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

    // ---------------- 時計からの要求 ----------------

    /** EVT agent.request (定型質問のテキスト)。 */
    fun onRequest(link: WatchLink, id: Int, text: String) {
        scope.launch {
            reply(link, id) { engine.answerText(text) }
        }
    }

    /** BULK kind="agent_audio" (旧経路: 音声入力の完了後に一括で届く ADP1)。 */
    fun onBulk(link: WatchLink, t: IncomingBulk) {
        if (t.kind != "agent_audio") return
        scope.launch {
            reply(link, t.id) { engine.answerAudio(t.bytes) }
        }
    }

    /** dictation の BULK_START を受けた。ASR を先に開いておく。 */
    fun onDictationStart(link: WatchLink, id: Int) {
        if (sessions.containsKey(id)) return
        val cfg = _config.value
        Log.i(TAG, "dictation start id=$id (${AiRouting.describe(cfg, voskReady())})")
        sessions[id] = newSession(link, id)
        link.setStreamingMode(true)
    }

    /** dictation の PCM (16bit LE / 16kHz / mono) が届いた。 */
    fun onDictationPcm(link: WatchLink, id: Int, pcm: ByteArray) {
        val s = sessions[id] ?: run {
            onDictationStart(link, id)
            sessions[id] ?: return
        }
        s.onPcm(pcm)
    }

    /** dictation の BULK_END。確定 → Muse 送信 → agent.reply を非同期に流す。 */
    fun onDictationEnd(link: WatchLink, id: Int) {
        val s = sessions.remove(id) ?: return
        scope.launch {
            try {
                s.finish()
            } finally {
                logDictationStats(id, s)
            }
        }
    }

    /**
     * BLE が切れた (自動再接続中): 進行中の dictation を ASR 側で確定し、
     * 返答は接続が戻ったら送れるよう保持する (protocol-v1.md §BULK dictation)。
     */
    fun onLinkLost() {
        val active = sessions.toMap()
        sessions.clear()
        for ((id, s) in active) {
            scope.launch {
                try {
                    s.finish()
                } catch (e: Exception) {
                    Log.w(TAG, "dictation finalize failed: ${e.message}")
                } finally {
                    logDictationStats(id, s)
                }
            }
        }
        Log.i(TAG, "link lost: ${active.size} dictation session(s) finalizing")
    }

    /** 接続先が切り替わった (FakeWatch ↔ BLE)。古い宛先の返答は捨てる。 */
    fun onLinkReset() {
        val active = sessions.toMap()
        sessions.clear()
        for (s in active.values) s.cancel()
        synchronized(pendingReplies) { pendingReplies.clear() }
        Log.i(TAG, "link reset: sessions cancelled")
    }

    /** 再接続時: 切断中に出せなかった返答を順に送る。 */
    fun flushPendingReplies(link: WatchLink) {
        val ready = pendingReplies.filter { it.link === link }
        if (ready.isEmpty()) return
        pendingReplies.removeAll(ready)
        scope.launch {
            for (p in ready) deliverReply(p.link, p.id, p.text)
        }
    }

    private fun newSession(link: WatchLink, id: Int): DictationSession {
        val cfg = _config.value
        val plan = AiRouting.sttPlan(cfg, voskReady())
        // STT エンジン: 既定はスマホ内 Vosk (無料)。Meta realtime は上級者向け。
        val sttFactory: ((onPartial: (String) -> Unit, onError: (String) -> Unit) -> SttEngine)? =
            when (plan) {
                SttPlan.MUSE_REALTIME -> if (cfg != null && cfg.metaAsrUsable()) {
                    { onPartial, onError ->
                        MetaRealtimeStt(
                            apiKey = cfg.metaApiKey,
                            languageBias = cfg.languageBias,
                            keywords = cfg.asrKeywords,
                            onPartial = onPartial,
                            onError = onError,
                        )
                    }
                } else {
                    null
                }
                SttPlan.VOSK -> createVoskStt?.let { creator ->
                    { onPartial, onError ->
                        creator(onPartial, onError)
                            ?: throw IllegalStateException(
                                "設定で音声モデルをダウンロードしてください",
                            )
                    }
                }
                else -> null
            }
        val batch: (suspend (ByteArray) -> String?)? = if (plan == SttPlan.OPENAI_BATCH) {
            { pcm: ByteArray -> engine.transcribePcm(pcm) }
        } else {
            null
        }
        val notReady = when (plan) {
            SttPlan.VOSK_NOT_READY -> AiRouting.VOSK_MODEL_MISSING
            else -> AgentEngine.NO_KEY_MESSAGE
        }
        var session: DictationSession? = null
        val created = DictationSession(
            id = id,
            createStt = sttFactory,
            batchTranscribe = batch,
            ask = { text -> engine.answerText(text) },
            onPartial = { text, final -> sendPartial(link, id, text, final) },
            onReply = { text -> sendReply(link, id, text) },
            scope = scope,
            notReadyMessage = notReady,
            onFinished = {
                // 音声が終わったら即 BLE の接続優先度を戻す (Muse の返答待ちは不要)。
                link.setStreamingMode(false)
                val s = session
                if (s != null) sessions.remove(id, s)
            },
        )
        session = created
        return created.also { it.start() }
    }

    private suspend fun sendPartial(link: WatchLink, id: Int, text: String, final: Boolean) {
        val res = try {
            link.request(Req.AgentPartial(id, text, final))
        } catch (e: Exception) {
            return
        }
        if (res is Res.Err) Log.w(TAG, "agent.partial failed: ${res.code} ${res.message}")
    }

    /** agent.reply を送る。未接続なら再接続時に送るため保持する。 */
    private suspend fun sendReply(link: WatchLink, id: Int, text: String) {
        if (link.state.value !is LinkState.Connected) {
            queueReply(link, id, text)
            return
        }
        val res = link.request(Req.AgentReply(id, truncateUtf8(text, AgentEngine.REPLY_MAX_BYTES)))
        if (res is Res.Err) {
            Log.w(TAG, "agent.reply failed: ${res.code} ${res.message}")
            // 切断由来なら接続が戻ったときに再送する。
            if (link.state.value !is LinkState.Connected) queueReply(link, id, text)
        }
    }

    private suspend fun deliverReply(link: WatchLink, id: Int, text: String) {
        val res = link.request(Req.AgentReply(id, truncateUtf8(text, AgentEngine.REPLY_MAX_BYTES)))
        if (res is Res.Err) Log.w(TAG, "agent.reply retry failed: ${res.code} ${res.message}")
    }

    private fun queueReply(link: WatchLink, id: Int, text: String) {
        val pending = PendingReply(link, id, text)
        synchronized(pendingReplies) {
            if (pendingReplies.size >= PENDING_REPLY_MAX) pendingReplies.removeAt(0)
            pendingReplies += pending
        }
        Log.i(TAG, "agent.reply queued (offline) id=$id")
        // 再接続を待って自動送信する。flushPendingReplies と二重送信しないよう
        // リストからの remove に成功した側だけが送る。
        scope.launch {
            withTimeoutOrNull(PENDING_REPLY_WAIT_MS) {
                link.state.first { it is LinkState.Connected }
            } ?: return@launch
            val taken = synchronized(pendingReplies) {
                if (pendingReplies.remove(pending)) pending else null
            } ?: return@launch
            deliverReply(taken.link, taken.id, taken.text)
        }
    }

    private fun logDictationStats(id: Int, s: DictationSession) {
        val st = s.stats()
        val sec = st.durationMs / 1000.0
        val pcmPerSec = if (sec > 0) (st.pcmBytes / sec).toLong() else 0
        Log.i(
            TAG,
            "dictation id=$id pcm=${st.pcmBytes}B ${sec}s (${pcmPerSec} B/s, " +
                "要求 32,000 B/s) dropped=${st.droppedBytes}B" +
                (if (st.batchTruncated) " batch_truncated" else ""),
        )
    }

    /** WAV を文字起こしするだけ (時計の音声メモ, T31)。 */
    suspend fun transcribeWav(wav: ByteArray): String? {
        val c = _config.value
        Log.i(
            TAG,
            "transcribe: url=${c.baseUrl} model=${c.sttModel} " +
                "keyLen=${c.apiKey.length} wav=${wav.size}B",
        )
        val text = mutex.withLock { engine.transcribeOnly(wav) }
        Log.i(TAG, "transcribe result: ${text?.length ?: -1} chars")
        return text
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
        sendReply(link, id, text)
    }

    // ---------------- Muse チャット画面の操作 ----------------

    fun refreshMuseChats() {
        val cfg = _config.value
        if (!cfg.museUsable()) {
            _museNotice.value = MuseApi.MUSE_TOKEN_MISSING
            return
        }
        if (_museLoading.value) return
        _museLoading.value = true
        scope.launch {
            try {
                _museChats.value = museApi.listChats(cfg.museToken, cfg.museEmail)
                if (_museChatId.value.isBlank() && cfg.museChatId.isNotBlank()) {
                    openMuseChat(cfg.museChatId)
                }
            } catch (e: MuseException) {
                _museNotice.value = e.japanese
            } catch (e: Exception) {
                _museNotice.value = "Muse の会話一覧を取得できませんでした"
            } finally {
                _museLoading.value = false
            }
        }
    }

    /** 会話を開く。まずアプリ側の履歴を表示し、取得できたらサーバの履歴で置き換える。 */
    fun openMuseChat(chatId: String) {
        _museChatId.value = chatId
        _museMessages.value = loadLocal(chatId)
        _museApproval.value = null
        if (chatId.isBlank()) return
        val cfg = _config.value
        if (!cfg.museUsable()) return
        scope.launch {
            try {
                val detail = museApi.readChat(cfg.museToken, chatId)
                val lines = detail.messages
                    .filter { it.text.isNotBlank() }
                    .map { MuseUiMessage(it.role, it.text) }
                if (lines.isNotEmpty()) {
                    _museMessages.value = lines
                    saveLocal(chatId, lines)
                }
            } catch (e: Exception) {
                // 取得できなくてもアプリ側の履歴を出しておく。
                Log.w(TAG, "readChat failed: ${e.message}")
            }
        }
    }

    /** 新規会話 (最初の送信で chatId を作る)。 */
    fun newMuseChat() {
        _museChatId.value = ""
        _museMessages.value = emptyList()
        _museApproval.value = null
        _museNotice.value = null
    }

    /** チャット画面からテキスト送信 (日本語 IME 可)。 */
    fun sendMuseText(text: String) {
        val body = text.trim()
        if (body.isEmpty() || _museBusy.value) return
        val cfg = _config.value
        if (!cfg.museUsable()) {
            _museNotice.value = MuseApi.MUSE_TOKEN_MISSING
            return
        }
        _museNotice.value = null
        _museApproval.value = null
        _museBusy.value = true
        appendLine(MuseUiMessage("user", body))
        appendLine(MuseUiMessage("assistant", "", streaming = true))
        scope.launch {
            var chatId = _museChatId.value.ifBlank { cfg.museChatId }
            try {
                val r = museChatService.ask(
                    cfg, body,
                    onChatId = { id ->
                        chatId = id
                        prefs.saveMuseChatId(id)
                        _museChatId.value = id
                    },
                    onDelta = { acc -> updateStreaming(acc) },
                    onApproval = { approval ->
                        _museApproval.value = approval
                        appendSystemLine(MuseApi.APPROVAL_MESSAGE)
                    },
                )
                chatId = r.chatId.ifBlank { chatId }
                finishStreaming(r.text)
                if (r.approval != null) {
                    _museNotice.value = "スマホの Muse アプリで承認してください"
                }
            } catch (e: MuseException) {
                removeStreaming()
                _museNotice.value = e.japanese
            } catch (e: Exception) {
                removeStreaming()
                _museNotice.value = "通信エラーです"
            } finally {
                _museBusy.value = false
                if (chatId.isNotBlank()) {
                    _museChatId.value = chatId
                    saveLocal(chatId, _museMessages.value)
                    if (_config.value.museChatId != chatId) {
                        updateConfig(_config.value.copy(museChatId = chatId))
                    }
                }
            }
        }
    }

    fun dismissMuseApproval() {
        _museApproval.value = null
    }

    /** 表示中の会話の履歴を消す (アプリ側のみ)。 */
    fun clearMuseHistory() {
        val chatId = _museChatId.value
        _museMessages.value = emptyList()
        prefs.clearMuseMessages(chatId.ifBlank { "watch" })
    }

    // ---------------- Muse 履歴の反映 ----------------

    /** 時計からの dictation / 定型質問で Muse に送った往復を同じ履歴に出す。 */
    private fun appendWatchTurn(question: String, answer: String) {
        appendLine(MuseUiMessage("user", question, fromWatch = true))
        appendLine(MuseUiMessage("assistant", answer, fromWatch = true))
        val chatId = _museChatId.value.ifBlank { _config.value.museChatId }
        if (chatId.isNotBlank()) {
            _museChatId.value = chatId
            saveLocal(chatId, _museMessages.value)
        }
    }

    private fun onMuseChatIdCreated(chatId: String) {
        if (chatId.isBlank()) return
        prefs.saveMuseChatId(chatId)
        val cfg = _config.value
        if (cfg.museChatId != chatId) {
            _config.value = cfg.copy(museChatId = chatId)
            engine.config = _config.value
        }
        _museChatId.value = chatId
    }

    private fun appendLine(line: MuseUiMessage) {
        _museMessages.value = _museMessages.value + line
    }

    private fun appendSystemLine(text: String) {
        _museMessages.value = _museMessages.value + MuseUiMessage("system", text)
        saveLocal(_museChatId.value.ifBlank { _config.value.museChatId }, _museMessages.value)
    }

    private fun updateStreaming(text: String) {
        val cur = _museMessages.value
        val idx = cur.indexOfLast { it.streaming }
        if (idx < 0) return
        _museMessages.value = cur.toMutableList().also {
            it[idx] = it[idx].copy(text = text)
        }
    }

    private fun finishStreaming(text: String) {
        val cur = _museMessages.value
        val idx = cur.indexOfLast { it.streaming }
        if (idx < 0) {
            appendLine(MuseUiMessage("assistant", text))
            return
        }
        _museMessages.value = cur.toMutableList().also {
            it[idx] = it[idx].copy(text = text, streaming = false)
        }
    }

    private fun removeStreaming() {
        _museMessages.value = _museMessages.value.filterNot { it.streaming }
    }

    private fun loadLocal(chatId: String): List<MuseUiMessage> =
        prefs.loadMuseMessages(chatId.ifBlank { "watch" })
            .map { MuseUiMessage(it.role, it.text) }

    private fun saveLocal(chatId: String, lines: List<MuseUiMessage>) {
        prefs.saveMuseMessages(
            chatId.ifBlank { "watch" },
            lines.map { AgentPrefs.MuseLine(it.role, it.text, System.currentTimeMillis()) },
        )
    }
}
