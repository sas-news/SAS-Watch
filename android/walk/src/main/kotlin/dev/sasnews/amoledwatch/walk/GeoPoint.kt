package dev.sasnews.amoledwatch.walk

import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * 緯度・経度 (度)。
 *
 * GPX の lat/lon をそのまま持つ。範囲外 (lat: ±90 超, lon: ±180 超, NaN/Inf) は
 * コンストラクタで弾く（GPX パーサが GpxException に包み直す）。
 */
data class GeoPoint(val lat: Double, val lon: Double) {
    init {
        require(lat.isFinite() && lat >= -90.0 && lat <= 90.0) { "緯度が範囲外です: $lat" }
        require(lon.isFinite() && lon >= -180.0 && lon <= 180.0) { "経度が範囲外です: $lon" }
    }
}

/** 地球半径 [m] (IUGG 平均半径)。haversine と局所平面の両方で使う。 */
internal const val EARTH_RADIUS_M = 6_371_008.8

/** 2 点間の haversine 距離 [m]。 */
internal fun haversineM(a: GeoPoint, b: GeoPoint): Double {
    val lat1 = Math.toRadians(a.lat)
    val lat2 = Math.toRadians(b.lat)
    val dLat = lat2 - lat1
    val dLon = Math.toRadians(b.lon - a.lon)
    val h = sin(dLat / 2) * sin(dLat / 2) +
        cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2)
    return 2 * EARTH_RADIUS_M * Math.asin(sqrt(h.coerceIn(0.0, 1.0)))
}

/**
 * 局所平面近似 (equirectangular) で origin から見た東 x・北 y [m] を返す。
 * 歩行規模 (数十 km) では誤差は無視できる (docs/walk-event.md §5.1)。
 */
internal fun localOffsetM(origin: GeoPoint, p: GeoPoint): Pair<Double, Double> {
    val x = Math.toRadians(p.lon - origin.lon) * cos(Math.toRadians(origin.lat)) * EARTH_RADIUS_M
    val y = Math.toRadians(p.lat - origin.lat) * EARTH_RADIUS_M
    return x to y
}

/** ルート (折れ線) へ現在地を投影した結果。 */
data class RouteProjection(
    /** ルート始点から投影点までの距離 [m]。 */
    val doneM: Double,
    /** 最寄り線分までの距離 [m]。 */
    val offRouteM: Double,
)

/** 2 点の局所平面上の距離 [m] (デバッグ・テスト用)。 */
internal fun planarDistanceM(a: GeoPoint, b: GeoPoint): Double {
    val (dx, dy) = localOffsetM(a, b)
    return hypot(dx, dy)
}
