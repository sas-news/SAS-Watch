package dev.sasnews.amoledwatch.walk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Route / RouteProgress のテスト。
 * 赤道沿いの直線 (経度 0 → 0.003°) を使う。全長 = 333.585 m。
 */
class RouteProgressTest {

    private val total = 333.5852407005987

    private fun line() = Route(
        name = "直線",
        points = listOf(GeoPoint(0.0, 0.0), GeoPoint(0.0, 0.003)),
    )

    @Test
    fun `haversine total length`() {
        assertEquals(total, line().totalM, 1e-6)
    }

    @Test
    fun `midpoint projection gives half distance and zero off route`() {
        val progress = RouteProgress(line())
        val u = progress.update(GeoPoint(0.0, 0.0015))
        assertEquals(166.792, u.doneM, 0.01)
        assertEquals(0.0, u.offRouteM, 0.01)
        assertEquals(166.792, u.remainingM, 0.01)
        assertEquals(500, u.permille)
        assertNull(u.nextCheckpoint)
    }

    @Test
    fun `perpendicular offset becomes off route distance`() {
        val progress = RouteProgress(line())
        val u = progress.update(GeoPoint(0.0009, 0.0015))
        // 緯度 0.0009° = 100.075 m (局所平面近似)
        assertEquals(100.075, u.offRouteM, 0.05)
        assertEquals(166.792, u.doneM, 0.05)
    }

    @Test
    fun `done never decreases`() {
        val progress = RouteProgress(line())
        progress.update(GeoPoint(0.0, 0.002))
        val before = progress.doneM
        val after = progress.update(GeoPoint(0.0, 0.0005))
        assertTrue(progress.doneM >= before)
        assertEquals(before, after.doneM, 1e-9)
        // 逸脱距離は今回の投影の値（戻りすぎは大きく出てよい）
        assertEquals(0.0, after.offRouteM, 0.01)
    }

    @Test
    fun `projection beyond the end clamps to total`() {
        val progress = RouteProgress(line())
        val u = progress.update(GeoPoint(0.0, 0.0035))
        assertEquals(total, u.doneM, 0.01)
        assertEquals(0.0, u.remainingM, 0.0)
        assertEquals(1000, u.permille)
        // 終点から 0.0005° = 55.597 m 先
        assertEquals(55.597, u.offRouteM, 0.05)
    }

    @Test
    fun `next checkpoint is the nearest one ahead`() {
        val route = Route(
            name = "CP付き",
            points = listOf(GeoPoint(0.0, 0.0), GeoPoint(0.0, 0.003)),
            checkpoints = listOf(
                Checkpoint("後半", GeoPoint(0.0, 0.0025)),
                Checkpoint("前半", GeoPoint(0.0, 0.0015)),
                Checkpoint("  ", GeoPoint(0.0, 0.001)),
            ),
        )
        assertEquals(2, route.checkpointsOnRoute.size)
        assertEquals("前半", route.checkpointsOnRoute[0].name)

        val progress = RouteProgress(route)
        val atStart = progress.update(GeoPoint(0.0, 0.0005))
        assertEquals("前半", atStart.nextCheckpoint!!.name)
        assertEquals(111.195, atStart.nextCheckpointRemainingM, 0.05)

        val later = progress.update(GeoPoint(0.0, 0.002))
        assertEquals("後半", later.nextCheckpoint!!.name)
        assertEquals(55.597, later.nextCheckpointRemainingM, 0.05)

        val finished = progress.update(GeoPoint(0.0, 0.0028))
        assertNull(finished.nextCheckpoint)
    }

    @Test
    fun `permille rounds and clamps`() {
        val route = line()
        assertEquals(0, route.permille(0.0))
        assertEquals(333, route.permille(total / 3))
        assertEquals(1000, route.permille(total * 2))
    }

    @Test
    fun `initial done is kept for session restore`() {
        val progress = RouteProgress(line(), initialDoneM = 200.0)
        assertEquals(200.0, progress.doneM, 0.0)
        // それより手前に投影されても減らない
        val u = progress.update(GeoPoint(0.0, 0.001))
        assertEquals(200.0, u.doneM, 0.0)
    }

    @Test
    fun `route requires two points and positive length`() {
        assertTrue(
            runCatching { Route("x", listOf(GeoPoint(0.0, 0.0))) }
                .exceptionOrNull() is IllegalArgumentException,
        )
        assertTrue(
            runCatching {
                Route("x", listOf(GeoPoint(0.0, 0.0), GeoPoint(0.0, 0.0)))
            }.exceptionOrNull() is IllegalArgumentException,
        )
        assertTrue(
            runCatching {
                Route(" ", listOf(GeoPoint(0.0, 0.0), GeoPoint(0.0, 0.001)))
            }.exceptionOrNull() is IllegalArgumentException,
        )
    }
}
