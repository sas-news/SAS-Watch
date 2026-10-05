package dev.sasnews.amoledwatch.service

import android.app.Notification
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.WatchApp
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.ui.MainActivity

/**
 * 時計との接続を維持する Foreground Service（foregroundServiceType="connectedDevice"）。
 * 接続が張られている間だけ動かし、切断したら止める。
 */
class WatchService : Service() {

    companion object {
        const val CHANNEL_ID = "watch_link"
        private const val NOTIFICATION_ID = 1
        private const val EXTRA_STOP = "dev.sasnews.amoledwatch.STOP"

        fun start(context: Context) {
            ContextCompat.startForegroundService(context, Intent(context, WatchService::class.java))
        }

        fun stop(context: Context) {
            context.startService(Intent(context, WatchService::class.java).putExtra(EXTRA_STOP, true))
        }

        /** 接続状態が変わったときに通知文を更新する。 */
        fun update(context: Context) {
            val app = context.applicationContext as WatchApp
            val nm = context.getSystemService(NotificationManager::class.java)
            nm.notify(NOTIFICATION_ID, buildNotification(context, app))
        }

        private fun buildNotification(context: Context, app: WatchApp): Notification {
            val text = when (app.manager.linkState.value) {
                is LinkState.Connected -> context.getString(R.string.fgs_connected)
                is LinkState.Connecting -> context.getString(R.string.fgs_connecting)
                is LinkState.Bonding -> context.getString(R.string.fgs_connecting)
                is LinkState.Disconnected -> context.getString(R.string.fgs_disconnected)
            }
            val open = PendingIntent.getActivity(
                context, 0,
                Intent(context, MainActivity::class.java),
                PendingIntent.FLAG_IMMUTABLE,
            )
            return NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentTitle(context.getString(R.string.app_name))
                .setContentText(text)
                .setContentIntent(open)
                .setOngoing(true)
                .build()
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.getBooleanExtra(EXTRA_STOP, false) == true) {
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
            return START_NOT_STICKY
        }
        startForeground(
            NOTIFICATION_ID,
            buildNotification(this, application as WatchApp),
        )
        return START_STICKY
    }
}
