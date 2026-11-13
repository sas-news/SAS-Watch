package dev.sasnews.amoledwatch.walk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.cos
import kotlin.math.sin

/**
 * 周回・折返しルートの投影テスト (H1 回帰)。
 *
 * 赤道沿いで 1° = 111194.9266 m。距離 [m] を緯度経度へ直すヘルパーを使う。
 */
class RouteProjectionTest {

    private companion object {
        const val M_PER_DEG = 111_194.92664455873
        const val SIDE_DEG = 0.005
        const val LEG_DEG = 0.01
    }

    /** メートル (東=dx, 北=dy) を緯度経度にする（赤道基準）。 */
    private fun geo(dxM: Double, dyM: Double): GeoPoint =
        GeoPoint(dyM / M_PER_DEG, dxM / M_PER_DEG)

    /** 始点=終点の正方形ループ (1 辺 0.005° ≈ 556m、全長 ≈ 2224m)。北→東→南→西の順。 */
    private fun squareLoop(): Route {
        val sideM = SIDE_DEG * M_PER_DEG
        return Route(
            name = "周回",
            points = listOf(
                geo(0.0, 0.0),
                geo(0.0, sideM),
                geo(sideM, sideM),
                geo(sideM, 0.0),
                geo(0.0, 0.0),
            ),
        )
    }

    /**
     * 始点から半径 3〜20m の 36 方位ジッターで、done が 0〜20m 程度に収まること。
     * 修正前は終点側の線分に吸われて done ≈ 2220m になっていた。
     */
    @Test
    fun `loop start jitter never jumps to the end of the loop`() {
        val route = squareLoop()
        for (radius in listOf(3.0, 5.0, 8.0, 12.0, 20.0)) {
            for (deg in 0 until 36) {
                val a = deg * 2.0 * Math.PI / 36.0
                val point = geo(radius * cos(a), radius * sin(a))
                val update = RouteProgress(route).update(point)
                assertTrue(
                    "r=$radius deg=$deg done=${update.doneM}",
                    update.doneM <= 20.0 + 0.01,
                )
                // 進行方向と逆側のジッターは始点から半径ぶん離れる。
                assertTrue(
                    "r=$radius deg=$deg off=${update.offRouteM}",
                    update.offRouteM <= 20.0 + 0.01,
                )
            }
        }
    }

    /**
     * 旧実装の再現ケース: 始点から東 8m・北 3m。戻り側の線分 (距離 3m) が
     * いちばん近いため done ≈ 2221m に張り付いた。ヒント窓で始点付近に留まること。
     */
    @Test
    fun `closer return leg does not win at the start`() {
        val update = RouteProgress(squareLoop()).update(geo(8.0, 3.0))
        assertTrue("done=${update.doneM}", update.doneM <= 20.0)
        assertEquals(8.0, update.offRouteM, 1.0)
    }

    /** 折返し (往復) ルート: 折り返した後、復路として done が増えること。 */
    @Test
    fun `fold back route progresses along the return leg`() {
        val legM = LEG_DEG * M_PER_DEG
        val route = Route(
            name = "往復",
            points = listOf(
                geo(0.0, 0.0),
                geo(0.0, legM),
                geo(0.0, 0.0),
            ),
        )
        val progress = RouteProgress(route)

        assertEquals(500.0, progress.update(geo(0.0, 500.0)).doneM, 1.0)
        val turn = progress.update(geo(0.0, legM))
        assertEquals(legM, turn.doneM, 1.0)

        // 100m 戻る = 往路と同じ物理位置。ヒントがあるので復路として進む。
        val back = progress.update(geo(0.0, legM - 100.0))
        assertTrue("back=${back.doneM} turn=${turn.doneM}", back.doneM > turn.doneM)
        assertEquals(legM + 100.0, back.doneM, 3.0)
    }

    /** 周回ルートを普通に前進したとき done が単調に伸び、位置どおりになること。 */
    @Test
    fun `loop forward walk is monotonic and accurate`() {
        val route = squareLoop()
        val sideM = SIDE_DEG * M_PER_DEG
        val progress = RouteProgress(route)
        var last = -1.0
        for (d in listOf(50.0, 300.0, 500.0, sideM)) {
            val update = progress.update(geo(0.0, d))
            assertEquals(d, update.doneM, 1.0)
            assertTrue(update.doneM >= last)
            last = update.doneM
        }
        // 北東角を曲がって東へ (2 辺目)。
        assertEquals(sideM + 100.0, progress.update(geo(100.0, sideM)).doneM, 1.0)
        assertEquals(2 * sideM, progress.update(geo(sideM, sideM)).doneM, 1.0)
    }

    /** ヒント窓の外にいる場合は全体探索へフォールバックし、ヒント近傍なら窓内で決まる。 */
    @Test
    fun `hint window is used and fallback happens only when far off route`() {
        val route = Route(
            name = "直線",
            points = listOf(
                geo(0.0, 0.0),
                geo(500.0, 0.0),
                geo(1000.0, 0.0),
                geo(1500.0, 0.0),
                geo(2000.0, 0.0),
            ),
        )
        // ヒント 1000m、現在地 900m → 窓内の線分で done ≈ 900m
        assertEquals(900.0, route.project(geo(900.0, 0.0), hintDoneM = 1000.0).doneM, 1.0)
        // ヒント 1000m、現在地 100m → 窓内の候補から 400m 離れているので全体探索で done ≈ 100m
        assertEquals(100.0, route.project(geo(100.0, 0.0), hintDoneM = 1000.0).doneM, 1.0)
        // ヒント無し = 全体探索
        assertEquals(100.0, route.project(geo(100.0, 0.0)).doneM, 1.0)
    }

    /** 終盤 (残り 50m) では始点付近のジッターを「終点に近い」と解釈する（周回の宿命）。 */
    @Test
    fun `loop near finish interprets start jitter as the end`() {
        val route = squareLoop()
        val total = route.totalM
        val progress = RouteProgress(route, initialDoneM = total - 50.0)
        val update = progress.update(geo(8.0, 0.0))
        assertTrue("done=${update.doneM} total=$total", update.doneM > total - 100.0)
    }

    /**
     * 実際に同梱している sample.gpx と同じ座標での再現テスト。
     * 始点=終点の周回で、始点から 8〜15m の 36 方位ジッターが終点側へ吸われないこと。
     */
    @Test
    fun `bundled sample gpx start jitter stays small`() {
        val gpx = """
            <?xml version="1.0" encoding="UTF-8"?>
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <wpt lat="35.6840" lon="139.7640"><name>第1関門</name></wpt>
              <trk><name>サンプルルート（2.6 km 周回）</name><trkseg>
                <trkpt lat="35.6800" lon="139.7600"/>
                <trkpt lat="35.6820" lon="139.7620"/>
                <trkpt lat="35.6840" lon="139.7640"/>
                <trkpt lat="35.6870" lon="139.7640"/>
                <trkpt lat="35.6890" lon="139.7620"/>
                <trkpt lat="35.6890" lon="139.7590"/>
                <trkpt lat="35.6870" lon="139.7570"/>
                <trkpt lat="35.6840" lon="139.7570"/>
                <trkpt lat="35.6820" lon="139.7584"/>
                <trkpt lat="35.6800" lon="139.7600"/>
              </trkseg></trk>
            </gpx>
        """.trimIndent()
        val route = GpxParser.parse(gpx.toByteArray(Charsets.UTF_8), "sample")
        val start = route.points.first()
        val mPerDegLon = M_PER_DEG * cos(Math.toRadians(start.lat))
        for (radius in listOf(8.0, 15.0)) {
            for (deg in 0 until 36) {
                val a = deg * 2.0 * Math.PI / 36.0
                val dx = radius * cos(a)
                val dy = radius * sin(a)
                val point = GeoPoint(start.lat + dy / M_PER_DEG, start.lon + dx / mPerDegLon)
                val update = RouteProgress(route).update(point)
                assertTrue(
                    "r=$radius deg=$deg done=${update.doneM}",
                    update.doneM <= 20.0 + 0.01,
                )
            }
        }
    }
}
