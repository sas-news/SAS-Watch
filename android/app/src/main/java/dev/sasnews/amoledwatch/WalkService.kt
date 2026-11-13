package dev.sasnews.amoledwatch

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import dev.sasnews.amoledwatch.ui.MainActivity
import dev.sasnews.amoledwatch.walk.WalkSnapshot
import dev.sasnews.amoledwatch.walk.WalkStatus

/**
 * 歩行中の測位を続ける Foreground Service
 * (`foregroundServiceType="location"`, docs/walk-event.md §5.2)。
 *
 * - 開始はアプリが前面にある操作 ([WalkRepository.startWalk]) から
 * - 通知に進捗 % ・残り ・到着予想を出す
 * - プロセスが落ちても START_STICKY で再生成され、[WalkRepository.onServiceStarted] が
 *   保存済みセッションから測位を再開する
 */
class WalkService : Service() {

    companion object {
        private const val TAG = "WalkService"

        /** 通知チャンネル id（WatchApp で作成。接続維持のチャンネルとは別）。 */
        const val CHANNEL_ID = "walk_active"

        private const val NOTIFICATION_ID = 2

        @Volatile
        private var running = false

        fun isRunning(): Boolean = running

        fun start(context: Context) {
            try {
                ContextCompat.startForegroundService(
                    context,
                    Intent(context, WalkService::class.java),
                )
            } catch (e: Exception) {
                // Android 12+ はバックグラウンドからの FGS 開始を拒否する。
                // 開始は前面の操作から行う前提なので、ここに来たらログだけ残す。
                Log.w(TAG, "WalkService を開始できません: ${e.message}")
            }
        }

        fun stop(context: Context) {
            runCatching { context.stopService(Intent(context, WalkService::class.java)) }
                .onFailure { Log.w(TAG, "WalkService を停止できません: ${it.message}") }
        }

        /** 状態が変わったら通知を更新する（サービスが動いていなければ何もしない）。 */
        fun update(context: Context) {
            if (!running) return
            val app = context.applicationContext as WatchApp
            val notification = buildNotification(context, app.walk.snapshot.value)
            runCatching {
                context.getSystemService(NotificationManager::class.java)
                    .notify(NOTIFICATION_ID, notification)
            }.onFailure { Log.w(TAG, "通知を更新できません: ${it.message}") }
        }

        private fun buildNotification(context: Context, snapshot: WalkSnapshot): Notification {
            val text = when (snapshot.state) {
                WalkStatus.WALKING, WalkStatus.NO_FIX -> context.getString(
                    R.string.walk_fgs_progress,
                    snapshot.permille / 10,
                    formatWalkDistance(snapshot.remainM),
                    formatWalkClock(snapshot.etaEpoch),
                )
                WalkStatus.FINISHED -> context.getString(R.string.walk_fgs_finished)
                else -> context.getString(R.string.walk_fgs_idle)
            }
            val open = PendingIntent.getActivity(
                context,
                0,
                Intent(context, MainActivity::class.java),
                PendingIntent.FLAG_IMMUTABLE,
            )
            return NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentTitle(context.getString(R.string.walk_fgs_title))
                .setContentText(text)
                .setContentIntent(open)
                .setOnlyAlertOnce(true)
                .setOngoing(true)
                .build()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val app = application as WatchApp
        return try {
            startForeground(NOTIFICATION_ID, buildNotification(this, app.walk.snapshot.value))
            running = true
            app.walk.onServiceStarted()
            START_STICKY
        } catch (e: Exception) {
            // Android 14 では位置情報の FGS 開始に権限が必要。欠けていたら静かに止める。
            Log.w(TAG, "foreground にできません: ${e.message}")
            stopSelf()
            START_NOT_STICKY
        }
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }
}
