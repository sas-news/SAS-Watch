package dev.sasnews.amoledwatch.walk

/**
 * ルートファイルの読み込み口。拡張子ではなく中身で形式を自動判定する。
 *
 * - GPX (trkpt / rtept / wpt): Strava・Komoot・YAMAP・RideWithGPS・Garmin Connect など
 * - KML (LineString / gx:Track / Point の Placemark): Google マイマップ・Google Earth など
 * - GeoJSON (LineString / MultiLineString / Feature / FeatureCollection): geojson.io など
 *
 * どれも 2MB / 20,000 点まで。失敗は [GpxException] (メッセージは画面に出す)。
 */
object RouteFileParser {

    /** ルートファイルの最大サイズ [byte]。 */
    const val MAX_BYTES = GpxParser.MAX_BYTES

    /** 最大点数の上限 (ルート点 + チェックポイント)。 */
    const val MAX_POINTS = GpxParser.MAX_POINTS

    /** ルート名がファイルに無いときの既定名。 */
    const val DEFAULT_NAME = "ルート"

    /** これ以内の点間隔の別セグメントは 1 本のルートにつなぐ [m]。 */
    internal const val MERGE_GAP_M = 500.0

    private val ROOT_GPX = Regex("<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?gpx(?:\\s|>)")
    private val ROOT_KML = Regex("<(?:[A-Za-z_][A-Za-z0-9_.-]*:)?kml(?:\\s|>)")

    /**
     * ルートファイルを [Route] にする。
     *
     * @param fallbackName ルート名がファイルに無いときに使う名前（通常はファイル名）
     */
    fun parse(bytes: ByteArray, fallbackName: String = DEFAULT_NAME): Route {
        if (bytes.isEmpty()) throw GpxException("ルートファイルが空です")
        if (bytes.size > MAX_BYTES) {
            throw GpxException("ファイルが大きすぎます（上限 2MB / 実際 ${bytes.size} B）")
        }
        // 形式判定用の先頭だけ UTF-8 として読む (XML の encoding 宣言は各パーサが尊重する)。
        val head = bytes.decodeToString().trimStart('\uFEFF', ' ', '\t', '\r', '\n')
        return when {
            head.startsWith("{") || head.startsWith("[") -> GeoJsonParser.parse(head, fallbackName)
            head.startsWith("<") -> {
                val sniff = head.take(4096)
                when {
                    ROOT_GPX.containsMatchIn(sniff) -> GpxParser.parse(bytes, fallbackName)
                    ROOT_KML.containsMatchIn(sniff) -> KmlParser.parse(bytes, fallbackName)
                    else -> throw GpxException(
                        "対応していない XML です（GPX / KML を読み込めます）",
                    )
                }
            }
            else -> throw GpxException(
                "GPX / KML / GeoJSON のいずれでもありません（先頭が XML でも JSON でもない）",
            )
        }
    }

    /** 点数の上限を検査する (パーサが点列を作る前に呼ぶ)。 */
    internal fun checkPointLimit(routePoints: Int, checkpointCount: Int) {
        if (routePoints + checkpointCount > MAX_POINTS) {
            throw GpxException(
                "点が多すぎます（上限 $MAX_POINTS 点 / 実際 ${routePoints + checkpointCount} 点）",
            )
        }
    }
}

/** パーサが集めた「1 本の折れ線」。名前は Placemark / Feature の名前 (無ければ null)。 */
internal data class RouteSegment(val name: String?, val points: List<GeoPoint>)

/**
 * セグメント群を 1 本のルートにする。
 *
 * - 端点が [RouteFileParser.MERGE_GAP_M] 以内の別セグメントはつなぐ (KML の分割線など)
 * - つながらないセグメントが複数あるときは、いちばん長いものをルートとして選ぶ
 *   (地図ツールで別々に描いた線が混ざっている場合の安全側)
 */
internal fun buildRouteFromSegments(
    segments: List<RouteSegment>,
    checkpoints: List<Checkpoint>,
    fallbackName: String,
): Route {
    val groups = ArrayList<MutableList<GeoPoint>>()
    val groupNames = ArrayList<String?>()
    var totalPoints = 0
    for (segment in segments) {
        if (segment.points.size < 2) continue
        totalPoints += segment.points.size
        val last = groups.lastOrNull()
        if (last != null) {
            val gap = planarDistanceM(last.last(), segment.points.first())
            if (gap <= RouteFileParser.MERGE_GAP_M) {
                // 同じ地点から始まるセグメントは先頭を重複させない。
                last.addAll(if (gap < 1.0) segment.points.drop(1) else segment.points)
                if (groupNames.last() == null) groupNames[groupNames.size - 1] = segment.name
                continue
            }
        }
        groups.add(segment.points.toMutableList())
        groupNames.add(segment.name)
    }
    RouteFileParser.checkPointLimit(totalPoints, checkpoints.size)
    if (groups.isEmpty()) throw GpxException("ルートの点が2点未満です")

    var best = 0
    var bestLength = -1.0
    for (i in groups.indices) {
        val length = polylineLength(groups[i])
        if (length > bestLength) {
            bestLength = length
            best = i
        }
    }
    val points = groups[best]
    if (points.size < 2) throw GpxException("ルートの点が2点未満です")

    val name = (groupNames[best] ?: fallbackName).trim()
        .ifBlank { RouteFileParser.DEFAULT_NAME }
    return try {
        Route(name, points, checkpoints)
    } catch (e: IllegalArgumentException) {
        throw GpxException(e.message ?: "ルートを作れません", e)
    }
}

/** 折れ線の総距離 [m]。 */
internal fun polylineLength(points: List<GeoPoint>): Double {
    var total = 0.0
    for (i in 1 until points.size) total += haversineM(points[i - 1], points[i])
    return total
}

/** "1,2" 形式 (KML の coordinates) の 1 点を GeoPoint にする。 */
internal fun kmlCoordinatePoint(token: String): GeoPoint {
    val parts = token.split(',')
    if (parts.size < 2) throw GpxException("座標の形式が不正です: $token")
    val lon = parts[0].trim().toDoubleOrNull()
        ?: throw GpxException("経度が数値ではありません: $token")
    val lat = parts[1].trim().toDoubleOrNull()
        ?: throw GpxException("緯度が数値ではありません: $token")
    return try {
        GeoPoint(lat, lon)
    } catch (e: IllegalArgumentException) {
        throw GpxException(e.message ?: "座標が不正です", e)
    }
}

/** "lon lat [alt]" 形式 (KML gx:coord) の 1 点を GeoPoint にする。 */
internal fun spaceCoordinatePoint(text: String): GeoPoint {
    val parts = text.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
    if (parts.size < 2) throw GpxException("gx:coord の形式が不正です: $text")
    val lon = parts[0].toDoubleOrNull()
        ?: throw GpxException("経度が数値ではありません: $text")
    val lat = parts[1].toDoubleOrNull()
        ?: throw GpxException("緯度が数値ではありません: $text")
    return try {
        GeoPoint(lat, lon)
    } catch (e: IllegalArgumentException) {
        throw GpxException(e.message ?: "座標が不正です", e)
    }
}
