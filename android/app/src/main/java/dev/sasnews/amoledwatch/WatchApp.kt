package dev.sasnews.amoledwatch

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import dev.sasnews.amoledwatch.media.MediaBridge
import dev.sasnews.amoledwatch.service.WatchService

class WatchApp : Application() {

    lateinit var prefs: Prefs
        private set
    lateinit var manager: WatchLinkManager
        private set
    lateinit var mediaBridge: MediaBridge
        private set

    override fun onCreate() {
        super.onCreate()
        prefs = Prefs(this)
        manager = WatchLinkManager(this)
        mediaBridge = MediaBridge(this).also { it.start() }

        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(
                WatchService.CHANNEL_ID,
                getString(R.string.fgs_channel_name),
                NotificationManager.IMPORTANCE_LOW,
            ),
        )
    }
}
