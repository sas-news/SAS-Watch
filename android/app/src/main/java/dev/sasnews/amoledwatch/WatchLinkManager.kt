package dev.sasnews.amoledwatch

import android.bluetooth.BluetoothDevice
import android.util.Log
import dev.sasnews.amoledwatch.ble.BleWatchConnection
import dev.sasnews.amoledwatch.connection.FakeWatchConnection
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.connection.WatchLink
import dev.sasnews.amoledwatch.protocol.Adpcm
import dev.sasnews.amoledwatch.protocol.BulkSender
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.DeviceInfo
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.HelloResult
import dev.sasnews.amoledwatch.protocol.MediaCmd
import dev.sasnews.amoledwatch.protocol.MemoAudioInfo
import dev.sasnews.amoledwatch.protocol.MemoEntry
import dev.sasnews.amoledwatch.protocol.MemoInfo
import dev.sasnews.amoledwatch.protocol.MemoListResult
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.protocol.int
import dev.sasnews.amoledwatch.protocol.Res
import java.io.File
import dev.sasnews.amoledwatch.service.WatchService
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.shareIn
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * 時計との通信の司令塔。
 * - 接続先（BLE 実機 / FakeWatch）の切り替え
 * - 接続時の `hello` → `time.set` 自動送信
 * - Foreground Service（接続維持）の開始・停止
 * - EVT の分配（ログ、メディア操作など）
 */
@OptIn(ExperimentalCoroutinesApi::class)
class WatchLinkManager(private val app: WatchApp) {

    companion object {
        private const val TAG = "WatchLinkManager"
        private const val LOG_LIMIT = 100
        private const val THEME_MAX_ATTEMPTS = 3
        private const val THEME_RECONNECT_WAIT_MS = 35_000L
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    private val _link = MutableStateFlow<WatchLink?>(null)
    val link: StateFlow<WatchLink?> = _link

    /** 現在の接続状態（未接続なら Disconnected）。 */
    val linkState: StateFlow<LinkState> = _link
        .flatMapLatest { it?.state ?: flowOf(LinkState.Disconnected) }
        .stateIn(scope, SharingStarted.Eagerly, LinkState.Disconnected)

    /** 時計からの EVT（なければ流れない）。 */
    val events: SharedFlow<Evt> = _link
        .flatMapLatest { it?.events ?: flowOf() }
        .shareIn(scope, SharingStarted.Eagerly)

    /** 画面上部に出す一言ログ。 */
    private val _log = MutableStateFlow<List<String>>(emptyList())
    val log: StateFlow<List<String>> = _log

    /** メモ一覧 (memo.list)。null は未取得。 */
    private val _memos = MutableStateFlow<List<MemoEntry>?>(null)
    val memos: StateFlow<List<MemoEntry>?> = _memos

    /** hello の結果。 */
    private val _hello = MutableStateFlow<HelloResult?>(null)
    val hello: StateFlow<HelloResult?> = _hello

    /** device.info の最新値。 */
    private val _deviceInfo = MutableStateFlow<DeviceInfo?>(null)
    val deviceInfo: StateFlow<DeviceInfo?> = _deviceInfo

    /** settings.get で読んだ最新の設定。 */
    private val _settings = MutableStateFlow<Map<String, Cbor>?>(null)
    val settings: StateFlow<Map<String, Cbor>?> = _settings

    /** 画面に出す一時メッセージ（エラー/成功）。 */
    private val _notice = MutableStateFlow<String?>(null)
    val notice: StateFlow<String?> = _notice

    fun consumeNotice() {
        _notice.value = null
    }

    // ---------------- 接続 ----------------

    fun connectBle(device: BluetoothDevice) {
        log("BLE 接続を開始: ${device.address}")
        open(BleWatchConnection(app, device, scope))
    }

    fun connectFake() {
        log("仮想時計に接続")
        open(FakeWatchConnection(scope))
    }

    fun disconnect() {
        _link.value?.close()
        _link.value = null
        WatchService.stop(app)
    }

    /** 接続中のリンクが FakeWatch なら返す（デモボタン用）。 */
    fun fakeConnection(): FakeWatchConnection? = _link.value as? FakeWatchConnection

    private fun open(l: WatchLink) {
        _link.value?.close()
        _hello.value = null
        _deviceInfo.value = null
        _settings.value = null
        _memos.value = null
        _link.value = l
        WatchService.start(app)
        scope.launch {
            l.state.collect { st ->
                WatchService.update(app)
                if (st is LinkState.Connected) handshake(l)
            }
        }
        scope.launch {
            l.events.collect { evt -> onEvent(evt) }
        }
    }

    /** 接続時の自動処理: hello → time.set → device.info → settings.get。 */
    private var handshakeJob: kotlinx.coroutines.Job? = null
    private fun handshake(l: WatchLink) {
        if (handshakeJob?.isActive == true) return
        handshakeJob = scope.launch {
            val h = send(Req.Hello(app = BuildConfig.VERSION_NAME))
            if (h is Res.Ok) {
                _hello.value = HelloResult.fromCbor(h.result)
            }
            val tz = TimeZone.getDefault()
            val now = System.currentTimeMillis()
            val epoch = now / 1000
            val tzMin = tz.getOffset(now) / 60_000
            send(Req.TimeSet(epoch, tzMin))
            refreshDeviceInfo()
            refreshSettings()
            refreshMemos()
        }
    }

    // ---------------- EVT ----------------

    private fun onEvent(evt: Evt) {
        log("EVT ${evt.name}")
        when (evt) {
            is Evt.Battery -> _deviceInfo.value = _deviceInfo.value?.copy(battery = evt.level, charging = evt.charging)
            is Evt.MediaCommand -> app.mediaBridge.handle(evt.cmd)
            is Evt.TimerFinished -> _notice.value = "タイマーが終了しました"
            is Evt.MemoSaved -> {
                _notice.value = "メモが保存されました（id=${evt.id}）"
                scope.launch { refreshMemos() }
            }
            is Evt.MemoDeleted -> scope.launch { refreshMemos() }
            is Evt.AgentRequest -> {}
            is Evt.Unknown -> {}
        }
    }

    // ---------------- REQ 送信 ----------------

    suspend fun send(req: Req): Res {
        val l = _link.value ?: return Res.Err("internal", "未接続")
        log("→ ${req.method}")
        val res = l.request(req)
        when (res) {
            is Res.Ok -> log("← ${req.method}: ok")
            is Res.Err -> {
                log("← ${req.method}: ${res.code} ${res.message}")
                _notice.value = app.getString(R.string.request_error, "${res.code} ${res.message}")
            }
        }
        return res
    }

    suspend fun refreshDeviceInfo() {
        val res = send(Req.DeviceInfo)
        if (res is Res.Ok) _deviceInfo.value = DeviceInfo.fromCbor(res.result)
    }

    suspend fun refreshSettings() {
        val res = send(Req.SettingsGet())
        if (res is Res.Ok) _settings.value = (res.result as? Cbor.Cmap)?.value
    }

    suspend fun setSetting(key: String, value: Cbor) {
        val res = send(Req.SettingsSet(mapOf(key to value)))
        if (res is Res.Ok) {
            _settings.value = _settings.value?.plus(key to value)
            _notice.value = app.getString(R.string.settings_saved)
        }
    }

    suspend fun timerStart(seconds: Int) = send(Req.TimerStart(seconds))

    suspend fun timerStop() = send(Req.TimerStop)

    suspend fun memoSend(text: String) {
        val res = send(Req.MemoCreate(text))
        if (res is Res.Ok) {
            val id = (res.result as? Cbor.Cmap)?.int("id", -1)?.toInt() ?: -1
            _notice.value = app.getString(R.string.memo_sent, id)
        }
    }

    // ---------------- メモ ----------------

    suspend fun refreshMemos() {
        val res = send(Req.MemoList(0, 16))
        if (res is Res.Ok) _memos.value = MemoListResult.fromCbor(res.result)?.memos
    }

    /** テキストメモの本文を取る (一覧では本文を載せない)。 */
    suspend fun memoText(id: Int): String? {
        val res = send(Req.MemoGet(id))
        return if (res is Res.Ok) MemoInfo.fromCbor(res.result)?.text else null
    }

    suspend fun memoDelete(id: Int) {
        val res = send(Req.MemoDelete(id))
        if (res is Res.Ok) refreshMemos()
    }

    /** 音声メモの実体 (ADP1) をダウンロードして cache に書く。失敗なら null。 */
    private suspend fun memoAudioFile(id: Int): File? {
        val l = _link.value ?: return null
        val res = send(Req.MemoAudioGet(id))
        if (res !is Res.Ok) return null
        val info = MemoAudioInfo.fromCbor(res.result) ?: return null
        val bytes = l.fetchBulk(info.id, info.sha256) ?: run {
            _notice.value = "音声の転送に失敗しました"
            return null
        }
        val dir = File(app.cacheDir, "memo_audio").apply { mkdirs() }
        val f = File(dir, "memo_${info.id}.adp")
        f.writeBytes(bytes)
        return f
    }

    /** 音声メモを WAV に変換して共有用ファイルとして返す。 */
    suspend fun memoWavFile(id: Int): File? {
        val adp = memoAudioFile(id) ?: return null
        val wav = Adpcm.adp1ToWav(adp.readBytes()) ?: run {
            _notice.value = "音声の形式が壊れています"
            return null
        }
        val dir = File(app.cacheDir, "share").apply { mkdirs() }
        val out = File(dir, "memo_${id}.wav")
        out.writeBytes(wav)
        return out
    }

    suspend fun mediaState(title: String, artist: String, playing: Boolean) =
        send(Req.MediaState(title, artist, playing))

    suspend fun notifyPost(appPkg: String, title: String, body: String) =
        send(Req.NotifyPost(appPkg, title, body))

    /**
     * テーマパッケージを BULK (kind="theme") で送る。
     * transferId は内容の sha256 先頭2バイト — 同じデータなら同じ id になるので、
     * 切断後の再送は時計側の受領位置から再開する（protocol-v1.md BULK 章）。
     * 内部エラー（切断・タイムアウト）は再接続を待ってリトライ、時計の明示的な
     * 拒否（bad_request 等）はリトライしない。戻り値 = 転送成功か。
     */
    suspend fun sendTheme(bytes: ByteArray, onProgress: (Int, Int) -> Unit = { _, _ -> }): Boolean {
        val sha = MessageDigest.getInstance("SHA-256").digest(bytes)
        val transferId = (sha[0].toInt() and 0xFF) or ((sha[1].toInt() and 0xFF) shl 8)
        var attempt = 0
        while (attempt < THEME_MAX_ATTEMPTS) {
            attempt++
            val link = _link.value ?: return false
            val ch = link.bulk ?: return false
            val res = try {
                BulkSender().send(ch, "theme", bytes, transferId, onProgress = onProgress)
            } catch (e: Exception) {
                Res.Err("internal", e.message ?: "send failed")
            }
            when (res) {
                is Res.Ok -> {
                    log("BULK theme 転送完了 (id=$transferId)")
                    return true
                }
                is Res.Err -> {
                    log("BULK theme 転送失敗 ($attempt/$THEME_MAX_ATTEMPTS): ${res.code} ${res.message}")
                    if (res.code != "internal") return false
                    // 切断系 → 再接続を待って同じ id で再送（再開）
                    val reconnected = withTimeoutOrNull(THEME_RECONNECT_WAIT_MS) {
                        link.state.first { it is LinkState.Connected }
                    }
                    if (reconnected == null && _link.value === link) return false
                }
            }
        }
        return false
    }

    /** デモ用: 時計側からメディア操作が来たふりをする。 */
    fun simulateMediaCmd(cmd: MediaCmd) = fakeConnection()?.fake?.simulateMediaCmd(cmd)

    /** 仮想時計に音声メモを録ったふりをさせる (デバッグ画面から)。 */
    fun simulateVoiceMemo() = fakeConnection()?.fake?.simulateVoiceMemo()

    fun simulateBattery() {
        val level = (40..95).random()
        fakeConnection()?.fake?.simulateBattery(level)
        log("EVT battery（デモ発生）")
    }

    private fun log(msg: String) {
        val stamp = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        _log.value = (_log.value + "$stamp $msg").takeLast(LOG_LIMIT)
        Log.d(TAG, msg)
    }
}
