package dev.sasnews.amoledwatch.media

/**
 * `media.state` 送信を「同一抑制 + 最小間隔間引き + trailing」で絞る純粋ロジック。
 * Android 非依存にして JUnit から検証する。
 *
 * プレイヤーによっては PlaybackState の進行位置更新で 1 秒毎に
 * onPlaybackStateChanged が呼ばれる。そのまま BLE に流すと `media.state`
 * が ~1s (混雑時 ~300ms) で連発し GATT 書き込みの競合を招くため:
 * - 送信内容 (title/artist/playing) が前回送信と同一なら送らない
 * - 変化があっても最短 [minIntervalMs] 間隔に間引く
 * - 間引き中の最後の状態は必ず trailing で送る (取りこぼし無し)
 */
class MediaStateCoalescer(private val minIntervalMs: Long = MIN_INTERVAL_MS) {

    /** BLE に送る値のキー (Req.MediaState と同じ組)。 */
    data class State(val title: String, val artist: String, val playing: Boolean)

    sealed interface Decision {
        /** 送信不要 (前回送信または保留と同一)。 */
        data object Suppress : Decision

        /** 即送信してよい。 */
        data class SendNow(val state: State) : Decision

        /** 間引き中。sendAfterMs に trailing 送信を予約すること。 */
        data class Pending(val state: State, val sendAfterMs: Long) : Decision
    }

    private var lastSent: State? = null
    private var lastSentAt: Long = Long.MIN_VALUE
    private var pending: State? = null

    /** 新しい状態を通知する。送信すべきかどうかの判定を返す。 */
    @Synchronized
    fun offer(nowMs: Long, s: State): Decision {
        if (s == lastSent || s == pending) return Decision.Suppress
        // lastSentAt は Long.MIN_VALUE 始まり — 未送信時は差分を取らず即送信。
        if (pending == null &&
            (lastSent == null || nowMs - lastSentAt >= minIntervalMs)) {
            lastSent = s
            lastSentAt = nowMs
            return Decision.SendNow(s)
        }
        pending = s
        return Decision.Pending(s, lastSentAt + minIntervalMs)
    }

    /** trailing の送信タイミングで呼ぶ。送信すべき保留状態があれば返す。 */
    @Synchronized
    fun flush(nowMs: Long): State? {
        val p = pending ?: return null
        if (nowMs - lastSentAt < minIntervalMs) return null
        pending = null
        lastSent = p
        lastSentAt = nowMs
        return p
    }

    /** 切断・セッション破棄などで状態をリセットする。 */
    @Synchronized
    fun reset() {
        lastSent = null
        lastSentAt = Long.MIN_VALUE
        pending = null
    }

    companion object {
        const val MIN_INTERVAL_MS = 500L
    }
}
