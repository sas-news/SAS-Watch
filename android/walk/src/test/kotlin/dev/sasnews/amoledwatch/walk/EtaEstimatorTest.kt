package dev.sasnews.amoledwatch.walk

import org.junit.Assert.assertEquals
import org.junit.Test

/** EtaEstimator のしきい値と計算のテスト (docs/walk-event.md §5.1)。 */
class EtaEstimatorTest {

    @Test
    fun `unknown before 50m or 60s`() {
        // done が足りない
        assertEquals(EtaEstimate(0, 0), EtaEstimator.estimate(10_000, 9_000, 49.9, 500.0))
        // 経過が足りない
        assertEquals(EtaEstimate(0, 0), EtaEstimator.estimate(10_059, 10_000, 500.0, 500.0))
        // 未開始
        assertEquals(EtaEstimate(0, 0), EtaEstimator.estimate(10_000, 0, 500.0, 500.0))
        // 時計が戻った
        assertEquals(EtaEstimate(0, 0), EtaEstimator.estimate(9_000, 10_000, 500.0, 500.0))
    }

    @Test
    fun `eta uses pace of elapsed over done`() {
        // done 100m / elapsed 100s → pace 1 s/m。残り 300m → 300s 後
        val e = EtaEstimator.estimate(nowEpoch = 1_000, startedEpoch = 900, doneM = 100.0, remainingM = 300.0)
        assertEquals(1_300L, e.etaEpoch)
        assertEquals(1_000L, e.paceSPerKm)
    }

    @Test
    fun `threshold values are inclusive`() {
        val e = EtaEstimator.estimate(nowEpoch = 1_060, startedEpoch = 1_000, doneM = 50.0, remainingM = 50.0)
        assertEquals(1_120L, e.etaEpoch) // pace 1.2 s/m * 50m = 60s
        assertEquals(1_200L, e.paceSPerKm) // 1.2 s/m * 1000
    }

    @Test
    fun `remaining zero gives now`() {
        val e = EtaEstimator.estimate(nowEpoch = 5_000, startedEpoch = 4_000, doneM = 200.0, remainingM = 0.0)
        assertEquals(5_000L, e.etaEpoch)
        assertEquals(5_000L, e.paceSPerKm)
    }
}

/** WalkStatus の wire 変換。 */
class WalkStatusTest {
    @Test
    fun `wire mapping and unknown fallback`() {
        assertEquals("walking", WalkStatus.WALKING.wire)
        assertEquals(WalkStatus.NO_FIX, WalkStatus.of("no_fix"))
        assertEquals(WalkStatus.IDLE, WalkStatus.of("unknown"))
        assertEquals(WalkStatus.IDLE, WalkStatus.of(""))
    }
}
