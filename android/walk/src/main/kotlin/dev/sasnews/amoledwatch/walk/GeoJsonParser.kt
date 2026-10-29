package dev.sasnews.amoledwatch.walk

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser

/**
 * GeoJSON (RFC 7946) を [Route] にする。
 *
 * - ルート: `LineString` / `MultiLineString` / `Polygon` の外周
 * - チェックポイント: 名前付き `Point` (Feature の `properties.name` など)
 * - 入れ子: `Feature` / `FeatureCollection` / `GeometryCollection`
 * - 名前: ルートを含む Feature の `properties.name` → fallbackName
 *
 * 座標は `[経度, 緯度]` の順 (GeoJSON の仕様)。
 */
internal object GeoJsonParser {

    fun parse(text: String, fallbackName: String): Route {
        val root: JsonElement = try {
            JsonParser.parseString(text)
        } catch (e: Exception) {
            throw GpxException("GeoJSON を解析できません", e)
        }
        val segments = ArrayList<RouteSegment>()
        val checkpoints = ArrayList<Checkpoint>()
        walk(root, null, segments, checkpoints)
        if (segments.isEmpty()) {
            throw GpxException("GeoJSON にルートがありません（LineString / MultiLineString が必要）")
        }
        return buildRouteFromSegments(segments, checkpoints, fallbackName)
    }

    private fun walk(
        element: JsonElement?,
        outerName: String?,
        segments: MutableList<RouteSegment>,
        checkpoints: MutableList<Checkpoint>,
    ) {
        val obj = element as? JsonObject ?: return
        when (obj.get("type")?.takeIf { it.isJsonPrimitive }?.asString) {
            "FeatureCollection" -> {
                obj.getAsJsonArray("features")?.forEach {
                    walk(it, outerName, segments, checkpoints)
                }
            }

            "Feature" -> {
                val name = obj.getAsJsonObject("properties")
                    ?.get("name")?.takeIf { it.isJsonPrimitive }?.asString
                    ?: outerName
                walk(obj.get("geometry"), name, segments, checkpoints)
            }

            "GeometryCollection" -> {
                obj.getAsJsonArray("geometries")?.forEach {
                    walk(it, outerName, segments, checkpoints)
                }
            }

            "LineString" -> {
                val points = coordinatesToPoints(obj.getAsJsonArray("coordinates"))
                if (points.size >= 2) segments.add(RouteSegment(outerName, points))
            }

            "MultiLineString" -> {
                obj.getAsJsonArray("coordinates")?.forEach { line ->
                    val points = coordinatesToPoints(line as? JsonArray)
                    if (points.size >= 2) segments.add(RouteSegment(outerName, points))
                }
            }

            // 外周 (閉じたリング) をルートとして扱う。地図ツールで線を多角形にした場合の保険。
            "Polygon" -> {
                val ring = obj.getAsJsonArray("coordinates")?.firstOrNull() as? JsonArray
                val points = coordinatesToPoints(ring)
                if (points.size >= 2) segments.add(RouteSegment(outerName, points))
            }

            "Point" -> {
                val pair = obj.getAsJsonArray("coordinates")
                if (pair != null && pair.size() >= 2 && !outerName.isNullOrBlank()) {
                    val lon = pair[0].asDoubleOrNull()
                    val lat = pair[1].asDoubleOrNull()
                    if (lon != null && lat != null) {
                        val point = try {
                            GeoPoint(lat, lon)
                        } catch (e: IllegalArgumentException) {
                            throw GpxException(e.message ?: "座標が不正です", e)
                        }
                        checkpoints.add(Checkpoint(outerName, point))
                    }
                }
            }
        }
    }

    /** `[lon, lat, ...]` の配列を点列にする。型不正は GpxException。 */
    private fun coordinatesToPoints(array: JsonArray?): List<GeoPoint> {
        if (array == null) return emptyList()
        val points = ArrayList<GeoPoint>(array.size())
        for (item in array) {
            val pair = item as? JsonArray
                ?: throw GpxException("GeoJSON の座標が配列ではありません")
            if (pair.size() < 2) throw GpxException("GeoJSON の座標が2要素未満です")
            val lon = pair[0].asDoubleOrNull()
                ?: throw GpxException("GeoJSON の経度が数値ではありません")
            val lat = pair[1].asDoubleOrNull()
                ?: throw GpxException("GeoJSON の緯度が数値ではありません")
            points.add(
                try {
                    GeoPoint(lat, lon)
                } catch (e: IllegalArgumentException) {
                    throw GpxException(e.message ?: "座標が不正です", e)
                },
            )
        }
        return points
    }

    private fun JsonElement.asDoubleOrNull(): Double? =
        if (isJsonPrimitive && asJsonPrimitive.isNumber) asDouble else null
}
