package dev.sasnews.amoledwatch

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat

/**
 * 歩行中だけ GPS を動かす測位ラッパー (docs/walk-event.md §5.2)。
 *
 * - 依存追加なし: Play Services ではなく [LocationManager] の GPS_PROVIDER を使う
 * - 10 秒 / 10 m 間隔で更新（歩行中だけ動かす）
 * - コールバックはメインスレッド。位置情報の権限は呼び出し側 ([WalkRepository]) が確認する
 */
class WalkTracker(private val context: Context) {

    companion object {
        private const val TAG = "WalkTracker"

        /** 測位間隔 (ms)。docs/walk-event.md §1 の 10 秒。 */
        const val UPDATE_INTERVAL_MS = 10_000L

        /** 最小移動距離 (m)。 */
        const val UPDATE_DISTANCE_M = 10f

        /** 開始直後に使ってよい last known 測位の鮮度 (ms)。 */
        private const val LAST_KNOWN_MAX_AGE_MS = 60_000L
    }

    private val locationManager: LocationManager? =
        context.getSystemService(LocationManager::class.java)

    /** 測位が来たら呼ばれる (メインスレッド)。 */
    var onLocation: ((Location) -> Unit)? = null

    /** 測位を開始できない理由 (GPS オフ・権限なしなど)。 */
    var onUnavailable: ((String) -> Unit)? = null

    var isRunning: Boolean = false
        private set

    private val listener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            onLocation?.invoke(location)
        }
    }

    fun hasLocationPermission(): Boolean =
        ContextCompat.checkSelfPermission(
            context,
            Manifest.permission.ACCESS_FINE_LOCATION,
        ) == PackageManager.PERMISSION_GRANTED ||
            ContextCompat.checkSelfPermission(
                context,
                Manifest.permission.ACCESS_COARSE_LOCATION,
            ) == PackageManager.PERMISSION_GRANTED

    fun isGpsEnabled(): Boolean =
        runCatching {
            locationManager?.isProviderEnabled(LocationManager.GPS_PROVIDER) == true
        }.getOrDefault(false)

    /**
     * 測位を開始する。呼び出し側で権限を確認済みであること
     * (取得できないときは false を返し、[onUnavailable] に理由を渡す)。
     */
    @SuppressLint("MissingPermission")
    fun start(): Boolean {
        if (isRunning) return true
        val lm = locationManager
        if (lm == null) {
            onUnavailable?.invoke("位置情報サービスが使えません")
            return false
        }
        if (!hasLocationPermission()) {
            onUnavailable?.invoke("位置情報が許可されていません")
            return false
        }
        if (!isGpsEnabled()) {
            onUnavailable?.invoke("GPS がオフになっています")
            return false
        }
        return try {
            lm.requestLocationUpdates(
                LocationManager.GPS_PROVIDER,
                UPDATE_INTERVAL_MS,
                UPDATE_DISTANCE_M,
                listener,
                Looper.getMainLooper(),
            )
            isRunning = true
            // 開始直後の「測位を待っています」を短くするため、新しい last known は使う。
            lm.getLastKnownLocation(LocationManager.GPS_PROVIDER)
                ?.takeIf { System.currentTimeMillis() - it.time <= LAST_KNOWN_MAX_AGE_MS }
                ?.let { onLocation?.invoke(it) }
            true
        } catch (e: SecurityException) {
            Log.w(TAG, "位置情報の権限がありません: ${e.message}")
            onUnavailable?.invoke("位置情報が許可されていません")
            false
        } catch (e: Exception) {
            Log.w(TAG, "測位を開始できません: ${e.message}")
            onUnavailable?.invoke("測位を開始できません")
            false
        }
    }

    fun stop() {
        if (!isRunning) return
        isRunning = false
        runCatching { locationManager?.removeUpdates(listener) }
    }
}
