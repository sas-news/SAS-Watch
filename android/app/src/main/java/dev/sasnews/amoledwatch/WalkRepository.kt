package dev.sasnews.amoledwatch

import android.content.Context
import android.content.SharedPreferences
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.walk.Checkpoint
import dev.sasnews.amoledwatch.walk.EtaEstimator
import dev.sasnews.amoledwatch.walk.GeoPoint
import dev.sasnews.amoledwatch.walk.GpxException
import dev.sasnews.amoledwatch.walk.GpxParser
import dev.sasnews.amoledwatch.walk.Route
import dev.sasnews.amoledwatch.walk.RouteProgress
import dev.sasnews.amoledwatch.walk.WalkSnapshot
import dev.sasnews.amoledwatch.walk.WalkStatus
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.security.MessageDigest
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/** 保存済みルート 1 件。UI には [route] の点列ごと渡す。 */
data class StoredRoute(
    val id: String,
    val route: Route,
    val importedEpoch: Long,
) {
    val name: String get() = route.name
    val totalM: Double get() = route.totalM
}

/**
 * 歩く大会 (Walk) の司令塔。
 *
 * - ルート (GPX) の一覧・選択を `filesDir/walk/routes.json` と SharedPreferences に保存
 * - 歩行セッション (ルート id・開始時刻・最大 done_m) を SharedPreferences に保存し、
 *   プロセス再起動・Foreground Service 再生成後も再開する
 * - 歩行中だけ [WalkTracker] で測位し、[WalkSnapshot] を組み立てる
 * - 開始・終了・ルート外れの変化を [WatchLinkManager.notifyWalkState] で時計へ自発送信
 *
 * 計算 (投影・ETA) は Android 非依存の `:walk` モジュールに任せ、ここは状態と保存だけを持つ。
 */
class WalkRepository(private val app: WatchApp) {

    companion object {
        private const val TAG = "WalkRepository"

        /** 同時に保持するルート数の上限（routes.json を無限に太らせない）。 */
        const val MAX_ROUTES = 10

        private const val PREFS_NAME = "walk"
        private const val KEY_SELECTED_ROUTE = "selected_route_id"
        private const val KEY_SESSION_ROUTE = "session_route_id"
        private const val KEY_SESSION_STATE = "session_state"
        private const val KEY_SESSION_STARTED = "session_started_epoch"
        private const val KEY_SESSION_FINISHED = "session_finished_epoch"
        private const val KEY_SESSION_DONE = "session_max_done_m"
        private const val KEY_SESSION_ACCURACY = "session_accuracy_m"

        private const val SESSION_WALKING = "walking"
        private const val SESSION_FINISHED = "finished"
        private const val HEX = "0123456789abcdef"
    }

    private enum class SessionState { NONE, IDLE, WALKING, FINISHED }

    private val prefs: SharedPreferences =
        app.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val routesFile = File(app.filesDir, "walk/routes.json")
    private val tracker = WalkTracker(app)

    private val _routes = MutableStateFlow<List<StoredRoute>>(emptyList())

    /** 保存済みルート（新しい順）。 */
    val routes: StateFlow<List<StoredRoute>> = _routes

    private val _selectedRouteId = MutableStateFlow<String?>(null)

    /** 選択中のルート id。 */
    val selectedRouteId: StateFlow<String?> = _selectedRouteId

    private val _snapshot = MutableStateFlow(WalkSnapshot.NO_ROUTE)

    /** 時計へ渡す最新状態。 */
    val snapshot: StateFlow<WalkSnapshot> = _snapshot

    private val _currentPoint = MutableStateFlow<GeoPoint?>(null)

    /** 直近の現在地（ルートのスケッチ用。測位できていなければ null）。 */
    val currentPoint: StateFlow<GeoPoint?> = _currentPoint

    private val _busy = MutableStateFlow(false)

    /** GPX の読み込み中。 */
    val busy: StateFlow<Boolean> = _busy

    private val _error = MutableStateFlow<String?>(null)

    /** 画面に出すエラー（表示後に [clearError] で消す）。 */
    val error: StateFlow<String?> = _error

    private var progress: RouteProgress? = null
    private var sessionRoute: StoredRoute? = null
    private var sessionState = SessionState.NONE
    private var sessionStartedEpoch = 0L
    private var sessionFinishedEpoch = 0L
    private var sessionMaxDoneM = 0.0
    private var sessionAccuracyM = 0
    private var hasFix = false
    private var lastOffRouteM = 0.0
    private var offRouteWarning = false

    /** メインスレッド用スコープ (StateFlow・セッション更新)。 */
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** ファイル I/O 用。直列化して routes.json の書き込み順を保つ。 */
    private val ioDispatcher = Dispatchers.IO.limitedParallelism(1)

    /** IO スレッドでパースした GPX の結果。 */
    private data class ParsedGpx(val route: Route, val id: String)

    init {
        tracker.onLocation = { loc -> onLocation(loc) }
        tracker.onUnavailable = { msg -> _error.value = msg }
        // routes.json の読み込みは IO で行い、完了後に snapshot を更新する (M4)。
        scope.launch {
            val loaded = withContext(ioDispatcher) { readRoutesFile() }
            _routes.value = loaded
            restoreSession()
            updateSnapshot()
        }
    }

    // ---------------- 権限・状態 ----------------

    fun hasLocationPermission(): Boolean = tracker.hasLocationPermission()

    fun isWalking(): Boolean = sessionState == SessionState.WALKING

    fun clearError() {
        _error.value = null
    }

    /** 選択中のルート（無ければ null）。 */
    fun selectedRoute(): StoredRoute? {
        val selected = _selectedRouteId.value
        return _routes.value.firstOrNull { it.id == selected } ?: sessionRoute
    }

    // ---------------- ルート管理 ----------------

    /** SAF で選んだ GPX を取り込む（読み込み・パースは IO スレッドで行う）。 */
    fun importGpx(uri: Uri) {
        if (sessionState == SessionState.WALKING) {
            _error.value = app.getString(R.string.walk_err_route_locked)
            return
        }
        if (_busy.value) return
        scope.launch {
            _busy.value = true
            _error.value = null
            try {
                val parsed = withContext(ioDispatcher) {
                    val bytes = app.contentResolver.openInputStream(uri)?.use { input ->
                        readLimited(input, GpxParser.MAX_BYTES + 1)
                    } ?: throw GpxException("ファイルを開けませんでした")
                    // ルート名のフォールバックは document id ではなく SAF の表示名から (M6)。
                    val fallback = queryDisplayName(uri)?.let(::routeNameFromFileName)
                    ParsedGpx(GpxParser.parse(bytes, fallback ?: "ルート"), routeId(bytes))
                }
                addRoute(parsed.route, parsed.id)
            } catch (e: CancellationException) {
                throw e
            } catch (e: GpxException) {
                _error.value = app.getString(R.string.walk_import_error, e.message ?: "")
            } catch (e: Exception) {
                Log.w(TAG, "GPX の取り込みに失敗: ${e.message}")
                _error.value = app.getString(R.string.walk_import_error, e.message ?: "")
            } finally {
                _busy.value = false
            }
        }
    }

    /** アプリ同梱のサンプルルート (assets/walk/sample.gpx) を取り込む。 */
    fun importSample() {
        if (sessionState == SessionState.WALKING) {
            _error.value = app.getString(R.string.walk_err_route_locked)
            return
        }
        if (_busy.value) return
        scope.launch {
            _busy.value = true
            _error.value = null
            try {
                val parsed = withContext(ioDispatcher) {
                    val bytes = app.assets.open("walk/sample.gpx").use { it.readBytes() }
                    ParsedGpx(GpxParser.parse(bytes, "sample"), routeId(bytes))
                }
                addRoute(parsed.route, parsed.id)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                Log.w(TAG, "サンプルルートを読めません: ${e.message}")
                _error.value = app.getString(R.string.walk_import_error, e.message ?: "")
            } finally {
                _busy.value = false
            }
        }
    }

    /** ルートを選ぶ（歩行中は変更不可。終了後は同じルートなら結果を残す）。 */
    fun selectRoute(id: String) {
        if (sessionState == SessionState.WALKING) {
            _error.value = app.getString(R.string.walk_err_route_locked)
            return
        }
        val route = _routes.value.firstOrNull { it.id == id } ?: return
        if (sessionState == SessionState.FINISHED && sessionRoute?.id == id) return
        _selectedRouteId.value = id
        prefs.edit().putString(KEY_SELECTED_ROUTE, id).apply()
        resetSession()
        sessionRoute = route
        sessionState = SessionState.IDLE
        saveSession()
        updateSnapshot()
    }

    /** ルートを削除する（歩行中は不可）。選択中なら次のルートを選ぶ。 */
    fun deleteRoute(id: String) {
        if (sessionState == SessionState.WALKING) {
            _error.value = app.getString(R.string.walk_err_route_locked)
            return
        }
        if (_routes.value.none { it.id == id }) return
        _routes.value = _routes.value.filterNot { it.id == id }
        saveRoutes()
        if (sessionRoute?.id == id) resetSession()
        if (_selectedRouteId.value == id) {
            val next = _routes.value.firstOrNull()
            _selectedRouteId.value = next?.id
            prefs.edit().putString(KEY_SELECTED_ROUTE, next?.id).apply()
            if (next != null) {
                sessionRoute = next
                sessionState = SessionState.IDLE
            }
        }
        saveSession()
        updateSnapshot()
    }

    private fun addRoute(route: Route, id: String) {
        val existing = _routes.value.firstOrNull { it.id == id }
        val stored = existing ?: run {
            if (_routes.value.size >= MAX_ROUTES) {
                _error.value = app.getString(R.string.walk_routes_full, MAX_ROUTES)
                return
            }
            StoredRoute(id, route, nowEpoch()).also {
                _routes.value = _routes.value + it
                saveRoutes()
            }
        }
        _selectedRouteId.value = stored.id
        prefs.edit().putString(KEY_SELECTED_ROUTE, stored.id).apply()
        resetSession()
        sessionRoute = stored
        sessionState = SessionState.IDLE
        saveSession()
        updateSnapshot()
    }

    /** SAF の表示名 (OpenableColumns.DISPLAY_NAME) を引く。取れなければ null。 */
    private fun queryDisplayName(uri: Uri): String? = runCatching {
        app.contentResolver.query(
            uri,
            arrayOf(OpenableColumns.DISPLAY_NAME),
            null,
            null,
            null,
        )?.use { cursor ->
            val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (index >= 0 && cursor.moveToFirst()) cursor.getString(index) else null
        }
    }.getOrNull()

    /** ファイル名から拡張子を除いたルート名。空なら null。 */
    private fun routeNameFromFileName(name: String): String? =
        name.substringAfterLast('/').substringBeforeLast('.').trim().takeIf { it.isNotEmpty() }

    /** 内容の sha256 先頭 8 バイトを id にする（同じファイルは同じ id = 重複しない）。 */
    private fun routeId(bytes: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(bytes)
        return buildString {
            for (i in 0 until 8) {
                val b = digest[i].toInt() and 0xFF
                append(HEX[b ushr 4])
                append(HEX[b and 0x0F])
            }
        }
    }

    // ---------------- 歩行セッション ----------------

    /** 歩行を開始する（Android 14 の FGS 制約を守るため、アプリ前面の操作から呼ぶ）。 */
    fun startWalk() {
        if (sessionState == SessionState.WALKING) return
        if (!hasLocationPermission()) {
            _error.value = app.getString(R.string.walk_err_permission)
            return
        }
        val route = selectedRoute()
        if (route == null) {
            _error.value = app.getString(R.string.walk_err_no_route)
            return
        }
        sessionRoute = route
        sessionState = SessionState.WALKING
        sessionStartedEpoch = nowEpoch()
        sessionFinishedEpoch = 0
        sessionMaxDoneM = 0.0
        sessionAccuracyM = 0
        hasFix = false
        lastOffRouteM = 0.0
        offRouteWarning = false
        progress = RouteProgress(route.route)
        _currentPoint.value = null
        _selectedRouteId.value = route.id
        saveSession()
        updateSnapshot()
        // 前面の操作なので startForegroundService してよい (Android 14 の制約)。
        WalkService.start(app)
        if (!tracker.start()) {
            // 権限はあるが GPS がオフ等。セッションは no_fix として残す。
            updateSnapshot()
        }
        app.manager.notifyWalkState()
    }

    /** 歩行を終了する（結果は画面に残す）。 */
    fun finishWalk() {
        if (sessionState != SessionState.WALKING) return
        sessionState = SessionState.FINISHED
        sessionFinishedEpoch = nowEpoch()
        sessionMaxDoneM = progress?.doneM ?: sessionMaxDoneM
        tracker.stop()
        WalkService.stop(app)
        saveSession()
        updateSnapshot()
        app.manager.notifyWalkState()
    }

    /**
     * Foreground Service が (再) 生成・再起動されたときの面倒を見る。
     * セッション復元・画面復帰・START_STICKY 再生成からの測位再開はここに集約する (M3)。
     * 権限喪失や GPS OFF から復帰したときもリスナーを張り直して更新を取り戻す (M2)。
     */
    fun onServiceStarted() {
        if (sessionState == SessionState.WALKING && hasLocationPermission()) {
            if (tracker.isRunning) tracker.stop()
            tracker.start()
        }
        updateSnapshot()
    }

    /** 歩行中なのにサービスが落ちていたら起こす（画面復帰時用）。 */
    fun ensureService() {
        if (sessionState == SessionState.WALKING) WalkService.start(app)
    }

    /**
     * 画面が前面に戻ったとき。歩行中なら FGS を ensure して測位をサービス経由で再開する
     * (M2/M3)。GPS 設定を直して戻ってきた場合もここから復帰する。
     */
    fun onScreenResumed() {
        if (sessionState != SessionState.WALKING) return
        if (!hasLocationPermission()) return
        // サービスの onServiceStarted が tracker.start() を行う (通知なし測位を避ける)。
        WalkService.start(app)
    }

    /**
     * GPS OFF・権限喪失などで測位が止まったときの手動リトライ (M2)。
     * 停止→開始し、Foreground Service も ensure する。
     */
    fun retryTracking() {
        if (sessionState != SessionState.WALKING) return
        if (!hasLocationPermission()) {
            _error.value = app.getString(R.string.walk_err_permission)
            return
        }
        _error.value = null
        tracker.stop()
        tracker.start()
        WalkService.start(app)
        updateSnapshot()
    }

    private fun onLocation(location: android.location.Location) {
        if (sessionState != SessionState.WALKING) return
        val route = sessionRoute ?: return
        val point = GeoPoint(location.latitude, location.longitude)
        val prog = progress ?: RouteProgress(route.route).also { progress = it }
        val update = prog.update(point)
        sessionMaxDoneM = update.doneM
        lastOffRouteM = update.offRouteM
        sessionAccuracyM = if (location.hasAccuracy()) {
            location.accuracy.roundToInt().coerceAtLeast(0)
        } else {
            0
        }
        hasFix = true
        _currentPoint.value = point
        saveSession()
        updateSnapshot()
        WalkService.update(app)
        // ルート外れの状態が変わったときだけ時計へ自発送信する。
        val offRoute = update.offRouteM > WalkSnapshot.OFF_ROUTE_WARN_M
        if (offRoute != offRouteWarning) {
            offRouteWarning = offRoute
            app.manager.notifyWalkState()
        }
    }

    private fun resetSession() {
        sessionRoute = null
        sessionState = SessionState.NONE
        sessionStartedEpoch = 0
        sessionFinishedEpoch = 0
        sessionMaxDoneM = 0.0
        sessionAccuracyM = 0
        hasFix = false
        lastOffRouteM = 0.0
        offRouteWarning = false
        progress = null
        _currentPoint.value = null
    }

    // ---------------- スナップショット ----------------

    private fun updateSnapshot() {
        _snapshot.value = buildSnapshot(nowEpoch())
    }

    private fun buildSnapshot(now: Long): WalkSnapshot {
        val stored = sessionRoute ?: selectedRoute() ?: return WalkSnapshot.NO_ROUTE.copy(updatedEpoch = now)
        val route = stored.route
        val totalM = route.totalM.roundToLong()
        return when (sessionState) {
            SessionState.WALKING -> {
                val done = progress?.doneM ?: 0.0
                val remain = route.remainingM(done)
                val elapsed = (now - sessionStartedEpoch).coerceAtLeast(0)
                val eta = EtaEstimator.estimate(now, sessionStartedEpoch, done, remain)
                val next = progress?.let { route.nextCheckpoint(it.doneM) }
                WalkSnapshot(
                    state = if (hasFix) WalkStatus.WALKING else WalkStatus.NO_FIX,
                    route = route.name,
                    totalM = totalM,
                    doneM = done.roundToLong(),
                    remainM = remain.roundToLong(),
                    permille = route.permille(done),
                    elapsedS = elapsed,
                    etaEpoch = eta.etaEpoch,
                    paceSPerKm = eta.paceSPerKm,
                    startedEpoch = sessionStartedEpoch,
                    nextCp = next?.name ?: "",
                    nextCpM = next?.let { (it.distanceM - done).coerceAtLeast(0.0).roundToLong() } ?: 0L,
                    offRouteM = lastOffRouteM.roundToLong(),
                    accuracyM = sessionAccuracyM.toLong(),
                    updatedEpoch = now,
                    msg = when {
                        !hasFix -> app.getString(R.string.walk_msg_waiting_fix)
                        lastOffRouteM > WalkSnapshot.OFF_ROUTE_WARN_M ->
                            app.getString(R.string.walk_msg_off_route)
                        else -> ""
                    },
                )
            }

            SessionState.FINISHED -> {
                val done = sessionMaxDoneM.coerceIn(0.0, route.totalM)
                val elapsed = (sessionFinishedEpoch - sessionStartedEpoch).coerceAtLeast(0)
                val pace = if (done >= EtaEstimator.MIN_DONE_M && elapsed >= EtaEstimator.MIN_ELAPSED_S) {
                    (elapsed / done * 1000.0).roundToLong()
                } else {
                    0L
                }
                WalkSnapshot(
                    state = WalkStatus.FINISHED,
                    route = route.name,
                    totalM = totalM,
                    doneM = done.roundToLong(),
                    remainM = route.remainingM(done).roundToLong(),
                    permille = route.permille(done),
                    elapsedS = elapsed,
                    etaEpoch = 0,
                    paceSPerKm = pace,
                    startedEpoch = sessionStartedEpoch,
                    offRouteM = 0,
                    accuracyM = sessionAccuracyM.toLong(),
                    updatedEpoch = now,
                )
            }

            else -> WalkSnapshot(
                state = WalkStatus.IDLE,
                route = route.name,
                totalM = totalM,
                updatedEpoch = now,
            )
        }
    }

    // ---------------- 保存 (routes.json / SharedPreferences) ----------------

    /** routes.json を読む。IO スレッドから呼ぶこと。壊れた要素は捨てる。 */
    private fun readRoutesFile(): List<StoredRoute> {
        if (!routesFile.exists()) return emptyList()
        val loaded = ArrayList<StoredRoute>()
        runCatching {
            val root = JSONObject(routesFile.readText())
            val arr = root.optJSONArray("routes") ?: JSONArray()
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val pointsArr = o.optJSONArray("points") ?: continue
                if (pointsArr.length() < 2) continue
                val points = ArrayList<GeoPoint>(pointsArr.length())
                var invalid = false
                for (j in 0 until pointsArr.length()) {
                    val pair = pointsArr.optJSONArray(j) ?: continue
                    try {
                        points.add(GeoPoint(pair.getDouble(0), pair.getDouble(1)))
                    } catch (e: Exception) {
                        invalid = true
                        break
                    }
                }
                if (invalid || points.size < 2) continue
                val checkpoints = ArrayList<Checkpoint>()
                o.optJSONArray("cps")?.let { cps ->
                    for (j in 0 until cps.length()) {
                        val cp = cps.optJSONObject(j) ?: continue
                        val name = cp.optString("name").trim()
                        if (name.isEmpty()) continue
                        try {
                            checkpoints.add(
                                Checkpoint(name, GeoPoint(cp.getDouble("lat"), cp.getDouble("lon"))),
                            )
                        } catch (e: Exception) {
                            // 壊れた CP は捨てる
                        }
                    }
                }
                runCatching {
                    loaded.add(
                        StoredRoute(
                            id = o.getString("id"),
                            route = Route(o.optString("name", "ルート"), points, checkpoints),
                            importedEpoch = o.optLong("imported_epoch"),
                        ),
                    )
                }
            }
        }.onFailure { Log.w(TAG, "routes.json を読めません: ${it.message}") }
        return loaded
    }

    /** 現在のルート一覧を IO で保存する（UI スレッドをブロックしない）。 */
    private fun saveRoutes() {
        val routes = _routes.value
        scope.launch(ioDispatcher) { writeRoutesFile(routes) }
    }

    private fun writeRoutesFile(source: List<StoredRoute>) {
        val root = JSONObject()
        root.put("version", 1)
        val arr = JSONArray()
        for (stored in source) {
            val o = JSONObject()
            o.put("id", stored.id)
            o.put("name", stored.route.name)
            o.put("imported_epoch", stored.importedEpoch)
            val points = JSONArray()
            stored.route.points.forEach { p ->
                points.put(JSONArray().put(p.lat).put(p.lon))
            }
            o.put("points", points)
            val cps = JSONArray()
            stored.route.checkpointsOnRoute.forEach { cp ->
                cps.put(
                    JSONObject()
                        .put("name", cp.name)
                        .put("lat", cp.point.lat)
                        .put("lon", cp.point.lon),
                )
            }
            o.put("cps", cps)
            arr.put(o)
        }
        root.put("routes", arr)
        runCatching {
            routesFile.parentFile?.mkdirs()
            routesFile.writeText(root.toString())
        }.onFailure { Log.w(TAG, "routes.json を書けません: ${it.message}") }
    }

    private fun restoreSession() {
        val selectedId = prefs.getString(KEY_SELECTED_ROUTE, null)
        _selectedRouteId.value = selectedId?.takeIf { id -> _routes.value.any { it.id == id } }
        val sessionId = prefs.getString(KEY_SESSION_ROUTE, null)
        val stored = _routes.value.firstOrNull { it.id == sessionId }
            ?: _routes.value.firstOrNull { it.id == _selectedRouteId.value }
            ?: return
        sessionRoute = stored
        if (_selectedRouteId.value == null) _selectedRouteId.value = stored.id
        sessionStartedEpoch = prefs.getLong(KEY_SESSION_STARTED, 0)
        sessionFinishedEpoch = prefs.getLong(KEY_SESSION_FINISHED, 0)
        sessionMaxDoneM = prefs.getFloat(KEY_SESSION_DONE, 0f).toDouble()
        sessionAccuracyM = prefs.getInt(KEY_SESSION_ACCURACY, 0)
        sessionState = when (prefs.getString(KEY_SESSION_STATE, null)) {
            SESSION_WALKING -> SessionState.WALKING
            SESSION_FINISHED -> SessionState.FINISHED
            else -> SessionState.IDLE
        }
        if (sessionState == SessionState.WALKING) {
            // 再起動後も最大 done から再開し、% が戻らないようにする。
            progress = RouteProgress(stored.route, sessionMaxDoneM)
            // 測位はここでは開始しない (M3)。サービスを ensure し、その onServiceStarted で
            // 再開する。Activity.onResume より復元が遅れる場合もあるためここでも起こす
            // (WalkService.start は冪等。FGS 起動不可なら内部で握りつぶす)。
            if (hasLocationPermission()) WalkService.start(app)
        }
    }

    private fun saveSession() {
        prefs.edit().apply {
            putString(KEY_SESSION_ROUTE, sessionRoute?.id)
            when (sessionState) {
                SessionState.WALKING -> putString(KEY_SESSION_STATE, SESSION_WALKING)
                SessionState.FINISHED -> putString(KEY_SESSION_STATE, SESSION_FINISHED)
                else -> remove(KEY_SESSION_STATE)
            }
            putLong(KEY_SESSION_STARTED, sessionStartedEpoch)
            putLong(KEY_SESSION_FINISHED, sessionFinishedEpoch)
            putFloat(KEY_SESSION_DONE, sessionMaxDoneM.toFloat())
            putInt(KEY_SESSION_ACCURACY, sessionAccuracyM)
        }.apply()
    }

    private fun readLimited(input: InputStream, limit: Int): ByteArray {
        val out = ByteArrayOutputStream()
        val buf = ByteArray(16 * 1024)
        while (true) {
            val n = input.read(buf)
            if (n < 0) break
            out.write(buf, 0, n)
            if (out.size() > limit) break
        }
        return out.toByteArray()
    }

    private fun nowEpoch(): Long = System.currentTimeMillis() / 1000
}

// ---------------- 時計へ送る形 ----------------

/** [WalkSnapshot] を REQ `walk.state` にする。 */
fun WalkSnapshot.toReq(): Req.WalkState = Req.WalkState(
    state = state.wire,
    route = route,
    totalM = totalM,
    doneM = doneM,
    remainM = remainM,
    permille = permille,
    elapsedS = elapsedS,
    etaEpoch = etaEpoch,
    paceSPerKm = paceSPerKm,
    startedEpoch = startedEpoch,
    nextCp = nextCp,
    nextCpM = nextCpM,
    offRouteM = offRouteM,
    accuracyM = accuracyM,
    updatedEpoch = updatedEpoch,
    msg = msg,
)

// ---------------- 表示フォーマット (通知と画面で共用) ----------------

/** 距離の表示。1km 未満は m、以上は 0.1km 単位。 */
fun formatWalkDistance(meters: Long): String = if (meters >= 1000) {
    String.format(Locale.JAPAN, "%.1f km", meters / 1000.0)
} else {
    "$meters m"
}

/** epoch 秒を端末タイムゾーンの HH:mm にする。0 以下は「—」。 */
fun formatWalkClock(epochSeconds: Long): String {
    if (epochSeconds <= 0) return "—"
    return DateTimeFormatter.ofPattern("HH:mm", Locale.JAPAN)
        .format(Instant.ofEpochSecond(epochSeconds).atZone(ZoneId.systemDefault()))
}

/** 経過秒を H:MM:SS にする。 */
fun formatWalkElapsed(seconds: Long): String {
    val s = seconds.coerceAtLeast(0)
    return String.format(Locale.JAPAN, "%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60)
}

/** ペース (秒/km) を M:SS /km にする。0 以下は「—」。 */
fun formatWalkPace(secondsPerKm: Long): String {
    if (secondsPerKm <= 0) return "—"
    return String.format(
        Locale.JAPAN,
        "%d:%02d /km",
        secondsPerKm / 60,
        secondsPerKm % 60,
    )
}
