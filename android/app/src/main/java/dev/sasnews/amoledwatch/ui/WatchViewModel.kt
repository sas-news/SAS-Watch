package dev.sasnews.amoledwatch.ui

import android.app.Application
import android.bluetooth.BluetoothDevice
import android.content.pm.PackageManager
import android.os.Build
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.sasnews.amoledwatch.WatchApp
import dev.sasnews.amoledwatch.ble.BleScanner
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.notif.NotificationForwarder
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.DeviceInfo
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.HelloResult
import dev.sasnews.amoledwatch.protocol.MediaCmd
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

class WatchViewModel(app: Application) : AndroidViewModel(app) {

    private val watchApp = app as WatchApp
    val manager = watchApp.manager

    private val scanner = BleScanner(app)

    val linkState: StateFlow<LinkState> = manager.linkState
    val hello: StateFlow<HelloResult?> = manager.hello
    val deviceInfo: StateFlow<DeviceInfo?> = manager.deviceInfo
    val settings: StateFlow<Map<String, Cbor>?> = manager.settings
    val log: StateFlow<List<String>> = manager.log
    val notice: StateFlow<String?> = manager.notice
    val enabledPackages: StateFlow<Set<String>> = watchApp.prefs.enabledPackages
    val nowPlaying: StateFlow<dev.sasnews.amoledwatch.media.MediaBridge.NowPlaying?> =
        watchApp.mediaBridge.nowPlaying

    /** 画面表示用に EVT を文字列化して溜める。 */
    private val _recentEvents = MutableStateFlow<List<String>>(emptyList())
    val recentEvents: StateFlow<List<String>> = _recentEvents

    /** 接続しているリンクが FakeWatch か（デモボタン表示用）。 */
    val isFakeLink: StateFlow<Boolean> = manager.link
        .map { it is dev.sasnews.amoledwatch.connection.FakeWatchConnection }
        .stateIn(viewModelScope, SharingStarted.Eagerly, false)

    init {
        viewModelScope.launch {
            manager.events.collect { evt ->
                _recentEvents.value = (_recentEvents.value + describe(evt)).takeLast(30)
            }
        }
    }

    private fun describe(evt: Evt): String = when (evt) {
        is Evt.Battery -> "電池 ${evt.level}%（${if (evt.charging) "充電中" else "電池駆動"}）"
        is Evt.TimerFinished -> "タイマー終了"
        is Evt.MemoSaved -> "メモ保存 id=${evt.id}（${evt.kind}）"
        is Evt.MemoDeleted -> "メモ削除 id=${evt.id}"
        is Evt.MediaCommand -> "メディア操作: ${evt.cmd.wire}"
        is Evt.AgentRequest -> "Agent要求 id=${evt.id}: ${evt.text}"
        is Evt.Unknown -> "${evt.name}"
    }

    // ---------------- スキャン ----------------

    private val _found = MutableStateFlow<Map<String, BleScanner.Found>>(emptyMap())
    val found: StateFlow<List<BleScanner.Found>> = _found
        .map { it.values.sortedByDescending { f -> f.rssi } }
        .stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    private val _scanning = MutableStateFlow(false)
    val scanning: StateFlow<Boolean> = _scanning
    private var scanJob: Job? = null

    fun startScan() {
        if (scanJob?.isActive == true) return
        _found.value = emptyMap()
        scanJob = viewModelScope.launch {
            _scanning.value = true
            try {
                scanner.scan().collect { f ->
                    _found.value = _found.value + (f.device.address to f)
                }
            } finally {
                _scanning.value = false
            }
        }
    }

    fun stopScan() {
        scanJob?.cancel()
        scanJob = null
        _scanning.value = false
    }

    // ---------------- 接続 ----------------

    fun connect(device: BluetoothDevice) {
        stopScan()
        manager.connectBle(device)
    }

    fun connectFake() {
        stopScan()
        manager.connectFake()
    }

    fun disconnect() = manager.disconnect()

    // ---------------- 操作 ----------------

    fun refresh() = viewModelScope.launch {
        manager.refreshDeviceInfo()
        manager.refreshSettings()
    }

    fun saveSettings(values: Map<String, Cbor>) = viewModelScope.launch {
        for ((k, v) in values) manager.setSetting(k, v)
    }

    fun timerStart(seconds: Int) = viewModelScope.launch { manager.timerStart(seconds) }
    fun timerStop() = viewModelScope.launch { manager.timerStop() }
    fun memoSend(text: String) = viewModelScope.launch { manager.memoSend(text) }

    fun fakeBattery() = manager.simulateBattery()
    fun fakeMediaCmd(cmd: MediaCmd) = manager.simulateMediaCmd(cmd)
    fun fakeVoiceMemo() = manager.simulateVoiceMemo()

    fun consumeNotice() = manager.consumeNotice()

    // ---------------- メモ ----------------

    val memos: StateFlow<List<dev.sasnews.amoledwatch.protocol.MemoEntry>?> =
        manager.memos

    /** テキストメモの本文キャッシュ (memo.list は本文を載せないので別途取る)。 */
    private val _memoTexts = MutableStateFlow<Map<Int, String>>(emptyMap())
    val memoTexts: StateFlow<Map<Int, String>> = _memoTexts

    /** 再生中の音声メモ id。 */
    private val _playingMemoId = MutableStateFlow<Int?>(null)
    val playingMemoId: StateFlow<Int?> = _playingMemoId

    /** 音声の取得・変換中のメモ id。 */
    private val _memoBusyId = MutableStateFlow<Int?>(null)
    val memoBusyId: StateFlow<Int?> = _memoBusyId

    private var player: android.media.MediaPlayer? = null
    private var textFetchJob: Job? = null

    init {
        // 一覧が更新されたらテキストメモの本文を拾ってくる。
        viewModelScope.launch {
            manager.memos.collect { list ->
                textFetchJob?.cancel()
                if (list == null) {
                    _memoTexts.value = emptyMap()
                    return@collect
                }
                val missing = list.filter { it.kind == "text" && !_memoTexts.value.containsKey(it.id) }
                if (missing.isEmpty()) return@collect
                textFetchJob = viewModelScope.launch {
                    for (m in missing) {
                        manager.memoText(m.id)?.let { t ->
                            _memoTexts.value = _memoTexts.value + (m.id to t)
                        }
                    }
                }
            }
        }
    }

    fun refreshMemos() = viewModelScope.launch { manager.refreshMemos() }

    fun memoDelete(id: Int) = viewModelScope.launch { manager.memoDelete(id) }

    /** 音声メモの再生トグル (未キャッシュなら時計から取って WAV にして鳴らす)。 */
    fun memoPlayToggle(id: Int) {
        if (_playingMemoId.value == id) {
            player?.release()
            player = null
            _playingMemoId.value = null
            return
        }
        viewModelScope.launch {
            _memoBusyId.value = id
            val wav = manager.memoWavFile(id)
            _memoBusyId.value = null
            if (wav == null) return@launch
            player?.release()
            player = android.media.MediaPlayer().apply {
                setDataSource(wav.absolutePath)
                prepare()
                setOnCompletionListener { _playingMemoId.value = null }
                start()
            }
            _playingMemoId.value = id
        }
    }

    /** 音声メモを WAV にして共有 (成功したら onReady に File を渡す)。 */
    fun memoShare(id: Int, onReady: (java.io.File) -> Unit) {
        viewModelScope.launch {
            _memoBusyId.value = id
            val wav = manager.memoWavFile(id)
            _memoBusyId.value = null
            if (wav != null) onReady(wav)
        }
    }

    // ---------------- 通知転送 ----------------

    fun togglePackage(pkg: String, enabled: Boolean) =
        watchApp.prefs.setPackageEnabled(pkg, enabled)

    private val _notifAccess = MutableStateFlow(false)
    val notifAccess: StateFlow<Boolean> = _notifAccess

    fun refreshNotifAccess() {
        _notifAccess.value = NotificationForwarder.hasAccess(watchApp)
        if (_notifAccess.value) watchApp.mediaBridge.start()
    }

    /** インストール済みアプリ（ランチャーに出るもの）の (packageName, ラベル) 一覧。 */
    fun installedApps(): List<Pair<String, String>> {
        val pm = watchApp.packageManager
        val apps = if (Build.VERSION.SDK_INT >= 33) {
            pm.getInstalledApplications(PackageManager.ApplicationInfoFlags.of(0))
        } else {
            @Suppress("DEPRECATION")
            pm.getInstalledApplications(0)
        }
        return apps
            .map { it.packageName to pm.getApplicationLabel(it).toString() }
            .sortedBy { it.second.lowercase() }
    }
}
