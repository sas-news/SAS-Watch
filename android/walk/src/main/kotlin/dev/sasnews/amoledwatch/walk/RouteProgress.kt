package dev.sasnews.amoledwatch.walk

/**
 * 現在地 1 点をルートへ投影した進捗。
 * `doneM` は端末内で最大値を保持する（GPS の揺れで % が下がらないように）。
 */
data class RouteProgressUpdate(
    /** 始点からルート上を進んだ距離 [m]（単調増加）。 */
    val doneM: Double,
    /** ルート (最寄り線分) からの逸脱距離 [m]。 */
    val offRouteM: Double,
    /** 残り距離 [m]。 */
    val remainingM: Double,
    /** 進捗 0..1000。 */
    val permille: Int,
    /** 次に通るチェックポイント（無ければ null）。 */
    val nextCheckpoint: CheckpointOnRoute?,
    /** 次のチェックポイントまでの残り距離 [m]（無ければ 0）。 */
    val nextCheckpointRemainingM: Double,
)

/**
 * ルート進捗の計算状態。`update()` を現在地ごとに呼ぶ。
 *
 * - `done_m` は最大値を保持して単調増加（secured: 折り返し・揺れで戻らない）
 * - `off_route_m` は今回の投影での最寄り線分距離
 */
class RouteProgress(
    val route: Route,
    initialDoneM: Double = 0.0,
) {
    /** 始点から進んだ距離 [m]。単調増加。 */
    var doneM: Double = initialDoneM.coerceIn(0.0, route.totalM)
        private set

    /** 直近の投影でのルートからの逸脱距離 [m]。 */
    var offRouteM: Double = 0.0
        private set

    /** 直近の投影結果（未更新なら null）。 */
    var lastProjection: RouteProjection? = null
        private set

    fun reset() {
        doneM = 0.0
        offRouteM = 0.0
        lastProjection = null
    }

    /** 現在地を取り込んで進捗を返す。 */
    fun update(point: GeoPoint): RouteProgressUpdate {
        // 直前の done をヒントにして、周回・往復で別の区間に飛ばないようにする。
        val projection = route.project(point, hintDoneM = doneM)
        lastProjection = projection
        offRouteM = projection.offRouteM
        // GPS の揺れ・ルートの折り返しで done が減らないよう最大値を保持する。
        doneM = maxOf(doneM, projection.doneM)
        val next = route.nextCheckpoint(doneM)
        return RouteProgressUpdate(
            doneM = doneM,
            offRouteM = projection.offRouteM,
            remainingM = route.remainingM(doneM),
            permille = route.permille(doneM),
            nextCheckpoint = next,
            nextCheckpointRemainingM = next?.let { (it.distanceM - doneM).coerceAtLeast(0.0) } ?: 0.0,
        )
    }
}
