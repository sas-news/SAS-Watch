package dev.sasnews.amoledwatch.walk

/**
 * 歩く大会の状態。時計には [wire] の文字列で送る (docs/protocol-v1.md §歩く大会)。
 */
enum class WalkStatus(val wire: String) {
    /** ルート選択済み・未開始。 */
    IDLE("idle"),

    /** 歩行中（一度でも測位できた）。 */
    WALKING("walking"),

    /** 到着・終了済み。 */
    FINISHED("finished"),

    /** ルート未選択。 */
    NO_ROUTE("no_route"),

    /** 開始したが一度も測位できていない（権限なし・GPS 待ち）。 */
    NO_FIX("no_fix"),
    ;

    companion object {
        /** 未知の値は `idle` として扱う (時計側と同じ寛容さ)。 */
        fun of(wire: String): WalkStatus = entries.firstOrNull { it.wire == wire } ?: IDLE
    }
}

/**
 * 電話→時計に渡す状態。REQ `walk.state` の材料
 * (docs/walk-event.md §3.2 のキーに対応する)。
 */
data class WalkSnapshot(
    val state: WalkStatus = WalkStatus.IDLE,
    val route: String = "",
    val totalM: Long = 0,
    val doneM: Long = 0,
    val remainM: Long = 0,
    val permille: Int = 0,
    val elapsedS: Long = 0,
    val etaEpoch: Long = 0,
    val paceSPerKm: Long = 0,
    val startedEpoch: Long = 0,
    val nextCp: String = "",
    val nextCpM: Long = 0,
    val offRouteM: Long = 0,
    val accuracyM: Long = 0,
    val updatedEpoch: Long = 0,
    val msg: String = "",
) {
    companion object {
        /** ルート未設定の初期値。 */
        val NO_ROUTE = WalkSnapshot(state = WalkStatus.NO_ROUTE)

        /** ルート外れとみなす閾値 [m]（画面に警告を出す）。 */
        const val OFF_ROUTE_WARN_M = 60.0
    }
}
