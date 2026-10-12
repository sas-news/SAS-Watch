package dev.sasnews.amoledwatch

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/** インストール済みアプリ 1件。 */
data class InstalledApp(val pkg: String, val label: String)

/**
 * インストール済みアプリ一覧のプロセス内キャッシュ。
 * `getInstalledApplications` + ラベル取得は端末によっては数百ms〜秒級で、
 * 画面表示のたびに呼ぶとメインスレッドを止めるのでここに集約する。
 *
 * - `apps` は null = 未読込/読込中。再スキャン中は旧リストを出したままにする。
 * - パッケージの追加/削除/置換 (PACKAGE_ADDED 等) で stale にするだけで、
 *   実際の再スキャンは次に `ensureLoaded()` が呼ばれたタイミング。
 */
class InstalledAppsRepo(private val context: Context) {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _apps = MutableStateFlow<List<InstalledApp>?>(null)
    val apps: StateFlow<List<InstalledApp>?> = _apps

    private var stale = true
    private var scanning = false
    private var generation = 0L

    private val packageReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            // 変更の印を付けるだけ (実際の再スキャンは画面側が呼ぶ)。
            synchronized(this@InstalledAppsRepo) {
                stale = true
                generation++
            }
        }
    }

    init {
        val filter = IntentFilter(Intent.ACTION_PACKAGE_ADDED).apply {
            addAction(Intent.ACTION_PACKAGE_REMOVED)
            addAction(Intent.ACTION_PACKAGE_REPLACED)
            addDataScheme("package")
        }
        ContextCompat.registerReceiver(
            context, packageReceiver, filter, ContextCompat.RECEIVER_NOT_EXPORTED,
        )
    }

    /** 未取得 or stale のときだけ再スキャンを起動する。取得済みなら即 return。 */
    fun ensureLoaded() {
        val gen = synchronized(this) {
            if (scanning || (!stale && _apps.value != null)) return
            scanning = true
            generation
        }
        scope.launch {
            val list = scan()
            synchronized(this@InstalledAppsRepo) {
                // スキャン中にパッケージ変更があった (generation が進んだ)
                // ときは stale を落とさず次回に回す。
                if (gen == generation) stale = false
                scanning = false
                _apps.value = list
            }
        }
    }

    /** 再スキャン本体。IO スレッドから呼ぶこと。 */
    private fun scan(): List<InstalledApp> {
        val pm = context.packageManager
        val infos = if (Build.VERSION.SDK_INT >= 33) {
            pm.getInstalledApplications(PackageManager.ApplicationInfoFlags.of(0))
        } else {
            @Suppress("DEPRECATION")
            pm.getInstalledApplications(0)
        }
        return infos
            .map { InstalledApp(it.packageName, pm.getApplicationLabel(it).toString()) }
            .sortedBy { it.label.lowercase() }
    }
}
