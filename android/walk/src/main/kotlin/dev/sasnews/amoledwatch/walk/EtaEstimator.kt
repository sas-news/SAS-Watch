package dev.sasnews.amoledwatch.walk

import kotlin.math.roundToLong

/** 到着予想の計算結果。0 は「不明」を表す。 */
data class EtaEstimate(
    /** 到着予想時刻 (epoch 秒)。0 = 不明。 */
    val etaEpoch: Long,
    /** 平均ペース (秒/km)。0 = 不明。 */
    val paceSPerKm: Long,
)

/**
 * 経過時間と進捗距離から到着予想を出す (docs/walk-event.md §5.1)。
 *
 * `done_m >= 50` かつ `elapsed_s >= 60` のときだけ算出する。
 * `pace = elapsed / done`、`eta = now + remain * pace`。
 * 条件を満たさない間は 0（不明）を返し、時計は「—」を表示する。
 */
object EtaEstimator {
    const val MIN_DONE_M = 50.0
    const val MIN_ELAPSED_S = 60L

    fun estimate(
        nowEpoch: Long,
        startedEpoch: Long,
        doneM: Double,
        remainingM: Double,
    ): EtaEstimate {
        val elapsedS = (nowEpoch - startedEpoch).coerceAtLeast(0L)
        if (startedEpoch <= 0L || doneM < MIN_DONE_M || elapsedS < MIN_ELAPSED_S) {
            return EtaEstimate(0, 0)
        }
        val paceSecPerM = elapsedS.toDouble() / doneM
        return EtaEstimate(
            etaEpoch = nowEpoch + (remainingM.coerceAtLeast(0.0) * paceSecPerM).roundToLong(),
            paceSPerKm = (paceSecPerM * 1000.0).roundToLong(),
        )
    }
}
