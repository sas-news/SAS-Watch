package dev.sasnews.amoledwatch.media

import android.content.ComponentName
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.util.Log
import dev.sasnews.amoledwatch.WatchApp
import dev.sasnews.amoledwatch.notif.NotificationForwarder
import dev.sasnews.amoledwatch.protocol.MediaCmd
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/**
 * MediaSessionManager で再生中の曲を取り、`media.state` を時計へ送る。
 * 逆方向（時計 → スマホの `media.cmd` EVT）で再生/停止/次/前/音量を操作する。
 * 通知アクセス（NotificationListener）が必要。
 */
class MediaBridge(private val app: WatchApp) {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private val manager = app.getSystemService(MediaSessionManager::class.java)
    private val audio = app.getSystemService(AudioManager::class.java)
    private val component = ComponentName(app, NotificationForwarder::class.java)

    data class NowPlaying(val title: String, val artist: String, val playing: Boolean)

    private val _nowPlaying = MutableStateFlow<NowPlaying?>(null)
    val nowPlaying: StateFlow<NowPlaying?> = _nowPlaying

    private var active: MediaController? = null
    private var registered = false

    private val sessionsListener =
        MediaSessionManager.OnActiveSessionsChangedListener { controllers ->
            pick(controllers)
        }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) = push()
        override fun onPlaybackStateChanged(state: PlaybackState?) = push()
        override fun onSessionDestroyed() {
            pick(null)
        }
    }

    fun hasAccess(): Boolean = NotificationForwarder.hasAccess(app)

    fun start() {
        if (registered || !hasAccess()) return
        try {
            manager.addOnActiveSessionsChangedListener(sessionsListener, component)
            registered = true
            pick(manager.getActiveSessions(component))
        } catch (e: SecurityException) {
            Log.w(TAG, "media session access denied", e)
        }
    }

    private fun pick(controllers: MutableList<MediaController>?) {
        active?.unregisterCallback(controllerCallback)
        val list = controllers ?: try {
            if (hasAccess()) manager.getActiveSessions(component) else mutableListOf()
        } catch (e: SecurityException) {
            mutableListOf()
        }
        active = list.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: list.firstOrNull()
        active?.registerCallback(controllerCallback)
        push()
    }

    /** 現在の再生状態を時計へ送る。 */
    private fun push() {
        val c = active
        val md = c?.metadata
        val title = md?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = md?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: md?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST) ?: ""
        val playing = c?.playbackState?.state == PlaybackState.STATE_PLAYING
        val np = if (title.isEmpty() && artist.isEmpty()) null else NowPlaying(title, artist, playing)
        _nowPlaying.value = np
        if (np != null) {
            scope.launch { app.manager.mediaState(np.title, np.artist, np.playing) }
        }
    }

    /** 時計からの `media.cmd` EVT を実際のメディア操作に変換する。 */
    fun handle(cmd: MediaCmd) {
        Log.i(TAG, "media cmd: $cmd")
        val tc = active?.transportControls
        when (cmd) {
            MediaCmd.PLAY_PAUSE -> {
                if (active?.playbackState?.state == PlaybackState.STATE_PLAYING) tc?.pause() else tc?.play()
            }
            MediaCmd.NEXT -> tc?.skipToNext()
            MediaCmd.PREV -> tc?.skipToPrevious()
            MediaCmd.VOL_UP -> audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, 0)
            MediaCmd.VOL_DOWN -> audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0)
        }
    }

    fun stop() {
        if (registered) {
            manager.removeOnActiveSessionsChangedListener(sessionsListener)
            registered = false
        }
        scope.cancel()
    }

    companion object {
        private const val TAG = "MediaBridge"
    }
}
