package dev.sasnews.amoledwatch.walk

/**
 * KML (Google マイマップ / Google Earth など) を [Route] にする。
 *
 * - ルート: `<Placemark>` 内の `<LineString><coordinates>` と
 *   `<gx:Track><gx:coord>`（Google Earth の GPS ログ形式）
 * - チェックポイント: 名前付き `<Placemark>` の `<Point>`
 * - ルート名: ルートを持つ Placemark の `<name>` → Document の `<name>` → fallbackName
 * - 分割された線は端点が 500m 以内なら 1 本につなぐ (RouteFileParser)
 *
 * 対応外: KMZ (zip)。エラーにその旨を出す。
 */
internal object KmlParser {

    fun parse(bytes: ByteArray, fallbackName: String): Route {
        val doc = XmlSupport.parse(bytes)
        val root = doc.documentElement ?: throw GpxException("KML の要素がありません")
        if (XmlSupport.localName(root) != "kml") {
            throw GpxException("KML ファイルではありません (<${XmlSupport.localName(root)}>)")
        }

        val segments = ArrayList<RouteSegment>()
        val checkpoints = ArrayList<Checkpoint>()
        val placemarks = XmlSupport.descendants(root)
            .filter { XmlSupport.localName(it) == "Placemark" }

        for (placemark in placemarks) {
            val name = XmlSupport.firstChildText(placemark, "name")
            val elements = XmlSupport.descendants(placemark)

            // ルート (LineString / gx:Track)
            for (line in elements.filter { XmlSupport.localName(it) == "LineString" }) {
                val text = XmlSupport.firstChildText(line, "coordinates") ?: continue
                val points = parseCoordinates(text)
                if (points.size >= 2) segments.add(RouteSegment(name, points))
            }
            for (track in elements.filter { XmlSupport.localName(it) == "Track" }) {
                val points = XmlSupport.children(track)
                    .filter { XmlSupport.localName(it) == "coord" }
                    .mapNotNull { child ->
                        val text = child.textContent?.trim().orEmpty()
                        if (text.isEmpty()) null else spaceCoordinatePoint(text)
                    }
                if (points.size >= 2) segments.add(RouteSegment(name, points))
            }

            // チェックポイント (名前付き Point)
            if (!name.isNullOrBlank()) {
                for (point in elements.filter { XmlSupport.localName(it) == "Point" }) {
                    val text = XmlSupport.firstChildText(point, "coordinates") ?: continue
                    val points = parseCoordinates(text)
                    if (points.size == 1) checkpoints.add(Checkpoint(name, points[0]))
                }
            }
        }

        if (segments.isEmpty()) {
            throw GpxException(
                "KML にルートがありません（LineString / gx:Track が必要。KMZ は解凍して KML にしてください）",
            )
        }
        return buildRouteFromSegments(
            segments,
            checkpoints,
            documentName(root) ?: fallbackName,
        )
    }

    private fun parseCoordinates(text: String): List<GeoPoint> {
        val tokens = text.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
        return tokens.map { kmlCoordinatePoint(it) }
    }

    /** `<Document><name>` (ルート名が無いときのフォールバック)。 */
    private fun documentName(root: org.w3c.dom.Element): String? {
        val document = XmlSupport.descendants(root)
            .firstOrNull { XmlSupport.localName(it) == "Document" }
            ?: return null
        return XmlSupport.firstChildText(document, "name")
    }
}
