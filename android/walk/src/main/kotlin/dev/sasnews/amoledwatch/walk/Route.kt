package dev.sasnews.amoledwatch.walk

import kotlin.math.hypot
import kotlin.math.roundToInt

/** GPX の `<wpt>` に対応するチェックポイント（名前つき地点）。 */
data class Checkpoint(val name: String, val point: GeoPoint)

/** ルート上の距離が分かっているチェックポイント（始点からの距離で昇順）。 */
data class CheckpointOnRoute(
    val name: String,
    val point: GeoPoint,
    /** ルート始点からこの CP までのルート上距離 [m]。 */
    val distanceM: Double,
)

/**
 * GPX から作るルート。点列・累積距離・チェックポイントを持つ。
 *
 * 距離は haversine、点列への投影は局所平面近似（[GeoPoint] 参照）。
 * 不変条件: 点が 2 点以上、全長 > 0、名前が空でない。
 */
class Route(
    val name: String,
    points: List<GeoPoint>,
    checkpoints: List<Checkpoint> = emptyList(),
) {
    /** ルートの点列（始点→終点）。 */
    val points: List<GeoPoint>

    /** 各点までのルート上距離 [m]。先頭は 0。 */
    val cumulativeM: DoubleArray

    /** ルート全長 [m]。 */
    val totalM: Double

    /** 名前のある `<wpt>` をルートへ投影したもの（距離の昇順）。 */
    val checkpointsOnRoute: List<CheckpointOnRoute>

    /** 局所平面の原点（始点）。 */
    private val origin: GeoPoint
    private val localX: DoubleArray
    private val localY: DoubleArray

    init {
        this.points = points.toList()
        require(this.points.size >= 2) { "ルートの点が2点以上必要です" }
        require(name.isNotBlank()) { "ルート名が空です" }

        origin = this.points.first()
        localX = DoubleArray(this.points.size)
        localY = DoubleArray(this.points.size)
        this.points.forEachIndexed { i, p ->
            val (x, y) = localOffsetM(origin, p)
            localX[i] = x
            localY[i] = y
        }

        cumulativeM = DoubleArray(this.points.size)
        for (i in 1 until this.points.size) {
            cumulativeM[i] = cumulativeM[i - 1] + haversineM(this.points[i - 1], this.points[i])
        }
        totalM = cumulativeM.last()
        require(totalM > 0.0) { "ルートの長さが 0 です（全点が同じ座標）" }

        checkpointsOnRoute = checkpoints
            .mapNotNull { cp ->
                val n = cp.name.trim()
                if (n.isEmpty()) return@mapNotNull null
                CheckpointOnRoute(n, cp.point, project(cp.point).doneM)
            }
            .sortedBy { it.distanceM }
    }

    /**
     * 現在地をルート (線分列) へ投影する。
     *
     * 周回・往復ルートでは同じ物理位置を 2 回以上通るため、全線分から無条件に最寄りを
     * 選ぶと、始点付近のジッターが終点付近の線分に吸われて done が一気に ~100% になる。
     * そこで [hintDoneM] (直前の done) の前後 [windowM] の線分だけを候補にする。
     * 候補の最寄り距離が [FALLBACK_OFF_ROUTE_M] より大きいとき（最初の測位・大きく
     * 外れたとき）だけ全体探索へフォールバックする。
     *
     * `doneM` はその線分始点の累積距離 + 線分内の割合、`offRouteM` は線分までの距離。
     */
    fun project(
        p: GeoPoint,
        hintDoneM: Double? = null,
        windowM: Double = HINT_WINDOW_M,
    ): RouteProjection {
        val (px, py) = localOffsetM(origin, p)

        if (hintDoneM != null && totalM > 0.0) {
            val lo = (hintDoneM - windowM).coerceAtLeast(0.0)
            val hi = (hintDoneM + windowM).coerceAtMost(totalM)
            val candidates = projectCandidates(px, py, lo, hi)
            if (candidates.isNotEmpty()) {
                val best = chooseCandidate(candidates, hintDoneM)
                if (best.offRouteM <= FALLBACK_OFF_ROUTE_M) {
                    return RouteProjection(best.doneM, best.offRouteM)
                }
            }
        }

        // 最初の測位・ヒントから大きく外れたときのみ全体探索（総当たり）。
        // 点数は最大 20,000 なので 10 秒に 1 回なら十分。
        var bestDone = 0.0
        var bestDist = Double.MAX_VALUE
        for (i in 0 until points.size - 1) {
            val (done, dist) = projectSegment(px, py, i)
            if (dist < bestDist) {
                bestDist = dist
                bestDone = done
            }
        }
        return RouteProjection(bestDone, bestDist)
    }

    private data class Candidate(val doneM: Double, val offRouteM: Double)

    /** 累積距離が [lo, hi] と交差する線分だけを投影する。 */
    private fun projectCandidates(px: Double, py: Double, lo: Double, hi: Double): List<Candidate> {
        val out = ArrayList<Candidate>()
        for (i in 0 until points.size - 1) {
            if (cumulativeM[i + 1] < lo || cumulativeM[i] > hi) continue
            val (done, dist) = projectSegment(px, py, i)
            out.add(Candidate(done, dist))
        }
        return out
    }

    private fun projectSegment(px: Double, py: Double, i: Int): Pair<Double, Double> {
        val ax = localX[i]
        val ay = localY[i]
        val dx = localX[i + 1] - ax
        val dy = localY[i + 1] - ay
        val len2 = dx * dx + dy * dy
        val t = if (len2 <= 0.0) {
            0.0
        } else {
            (((px - ax) * dx + (py - ay) * dy) / len2).coerceIn(0.0, 1.0)
        }
        val qx = ax + t * dx
        val qy = ay + t * dy
        val done = cumulativeM[i] + t * (cumulativeM[i + 1] - cumulativeM[i])
        return done to hypot(px - qx, py - qy)
    }

    /**
     * 候補のうち最も近い線分を選ぶ。同距離 ([TIE_DIST_M] 以内) が複数あるときは
     * 前進方向 (done >= hint) を優先し、その中で最も手前を選ぶ。折返しの折り返し点でも
     * 復路側へ自然に進める。
     */
    private fun chooseCandidate(candidates: List<Candidate>, hintDoneM: Double): Candidate {
        val minDist = candidates.minOf { it.offRouteM }
        val near = candidates.filter { it.offRouteM <= minDist + TIE_DIST_M }
        val ahead = near.filter { it.doneM >= hintDoneM - 1e-6 }
        return if (ahead.isNotEmpty()) {
            ahead.minByOrNull { it.doneM }!!
        } else {
            near.maxByOrNull { it.doneM }!!
        }
    }

    /** 始点から doneM [m] 進んだ時点の残り距離 [m]。 */
    fun remainingM(doneM: Double): Double = (totalM - doneM).coerceAtLeast(0.0)

    /** doneM より先にある最短のチェックポイント（無ければ null）。 */
    fun nextCheckpoint(doneM: Double): CheckpointOnRoute? =
        checkpointsOnRoute.firstOrNull { it.distanceM > doneM + CHECKPOINT_EPS_M }

    /** 進捗 0..1000 (done/total)。四捨五入。 */
    fun permille(doneM: Double): Int =
        ((doneM / totalM) * 1000.0).roundToInt().coerceIn(0, 1000)

    companion object {
        /** ヒント区間の既定幅 [m]。GPS 10 秒間隔 (歩行) なら十分。 */
        const val HINT_WINDOW_M = 500.0

        /** ヒント区間の候補がこれ以上離れていたら全体探索へフォールバックする [m]。 */
        const val FALLBACK_OFF_ROUTE_M = 150.0

        /** どちらも同じくらい近いとみなす距離差 [m]（折返し・周回の同一点対策）。 */
        private const val TIE_DIST_M = 2.0

        private const val CHECKPOINT_EPS_M = 1e-6
    }
}
