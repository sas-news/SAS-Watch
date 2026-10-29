package dev.sasnews.amoledwatch

import android.app.Application
import android.app.NotificationChannel
import android.app.NotificationManager
import dev.sasnews.amoledwatch.agent.AgentBridge
import dev.sasnews.amoledwatch.agent.AgentPrefs
import dev.sasnews.amoledwatch.media.MediaBridge
import dev.sasnews.amoledwatch.service.WatchService

class WatchApp : Application() {

    lateinit var prefs: Prefs
        private set
    lateinit var manager: WatchLinkManager
        private set
    lateinit var walk: WalkRepository
        private set
    lateinit var mediaBridge: MediaBridge
        private set
    lateinit var agentBridge: AgentBridge
        private set
    lateinit var installedApps: InstalledAppsRepo
        private set

    override fun onCreate() {
        super.onCreate()
        prefs = Prefs(this)
        agentBridge = AgentBridge(AgentPrefs(this))
        manager = WatchLinkManager(this)
        // 保存済みの歩行セッションを復元し、歩行中なら測位を再開する。
        walk = WalkRepository(this)
        mediaBridge = MediaBridge(this).also { it.start() }
        installedApps = InstalledAppsRepo(this)

        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(
                WatchService.CHANNEL_ID,
                getString(R.string.fgs_channel_name),
                NotificationManager.IMPORTANCE_LOW,
            ),
        )
        // 歩行中の測位は Foreground Service + 専用チャンネルで継続する (WalkService)。
        nm.createNotificationChannel(
            NotificationChannel(
                WalkService.CHANNEL_ID,
                getString(R.string.walk_channel_name),
                NotificationManager.IMPORTANCE_LOW,
            ),
        )
    }
}
