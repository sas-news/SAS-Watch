package dev.sasnews.amoledwatch.notif

import android.app.Notification
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.text.TextUtils
import android.util.Log
import dev.sasnews.amoledwatch.WatchApp
import dev.sasnews.amoledwatch.protocol.Req
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * 通知 → `notify.post` 転送と MediaSession 権限の受け皿。
 * 対象アプリは設定画面で選ぶ（Prefs.enabledPackages）。
 */
class NotificationForwarder : NotificationListenerService() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    override fun onListenerConnected() {
        Log.i(TAG, "notification listener connected")
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        val app = application as WatchApp
        if (sbn.packageName == packageName) return
        if (!app.prefs.enabledPackages().contains(sbn.packageName)) return
        if (sbn.isOngoing && sbn.notification.flags and Notification.FLAG_GROUP_SUMMARY == 0) {
            // 常駐通知（音楽プレイヤー等）はメディア連携側が面倒を見る // TODO(hw): 実機で確認 — どの通知を時計に出すか
            return
        }
        val extras = sbn.notification.extras
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()
            ?: sbn.packageName
        val body = extras.getCharSequence(Notification.EXTRA_TEXT)?.toString() ?: ""
        scope.launch {
            app.manager.notifyPost(appPkg = sbn.packageName, title = title, body = body)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        scope.cancel()
    }

    companion object {
        private const val TAG = "NotificationForwarder"

        /** 通知アクセスが許可されているか。 */
        fun hasAccess(context: android.content.Context): Boolean {
            val enabled = Settings.Secure.getString(
                context.contentResolver,
                "enabled_notification_listeners",
            ) ?: return false
            return enabled.split(':').any { component ->
                !TextUtils.isEmpty(component) && component.contains(context.packageName)
            }
        }
    }
}
