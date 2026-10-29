package dev.sasnews.amoledwatch.walk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * ルートファイル読み込み (GPX / KML / GeoJSON) のテスト。
 * 主に「地図ツールから書き出したファイル」を取り込めることを確認する。
 */
class RouteFileParserTest {

    // ---------- 形式の自動判定 ----------

    @Test
    fun `GPX は従来どおり読める`() {
        val gpx = """
            <?xml version="1.0" encoding="UTF-8"?>
            <gpx version="1.1" creator="test">
              <trk><name>33km コース</name><trkseg>
                <trkpt lat="35.6900" lon="139.7000"/>
                <trkpt lat="35.6910" lon="139.7010"/>
              </trkseg></trk>
            </gpx>
        """.trimIndent()
        val route = RouteFileParser.parse(gpx.toByteArray(), "fallback")
        assertEquals("33km コース", route.name)
        assertEquals(2, route.points.size)
    }

    @Test
    fun `空ファイルはエラー`() {
        assertThrows(GpxException::class.java) {
            RouteFileParser.parse(ByteArray(0))
        }
    }

    @Test
    fun `未知の XML はエラー`() {
        val e = assertThrows(GpxException::class.java) {
            RouteFileParser.parse("<foo><bar/></foo>".toByteArray())
        }
        assertTrue(e.message!!.contains("GPX / KML"))
    }

    @Test
    fun `XML でも JSON でもない中身はエラー`() {
        assertThrows(GpxException::class.java) {
            RouteFileParser.parse("lat,lon\n35.6,139.7\n".toByteArray())
        }
    }

    @Test
    fun `2MB を超えるファイルはエラー`() {
        val big = ByteArray(RouteFileParser.MAX_BYTES + 1) { ' '.code.toByte() }
        assertThrows(GpxException::class.java) { RouteFileParser.parse(big) }
    }

    // ---------- KML (Google マイマップ / Google Earth) ----------

    @Test
    fun `KML の LineString と名前付き Point を読める`() {
        val kml = """
            <?xml version="1.0" encoding="UTF-8"?>
            <kml xmlns="http://www.opengis.net/kml/2.2">
              <Document>
                <name>マイマップ</name>
                <Placemark>
                  <name>33km ウォーキング</name>
                  <LineString>
                    <coordinates>
                      139.7000,35.6900,0 139.7010,35.6910,0
                      139.7020,35.6920,0
                    </coordinates>
                  </LineString>
                </Placemark>
                <Placemark>
                  <name>第1関門</name>
                  <Point><coordinates>139.7010,35.6910,0</coordinates></Point>
                </Placemark>
                <Placemark>
                  <name>名前なし地点</name>
                  <Point><coordinates>139.7015,35.6915,0</coordinates></Point>
                </Placemark>
              </Document>
            </kml>
        """.trimIndent()
        val route = RouteFileParser.parse(kml.toByteArray(), "fallback")
        assertEquals("33km ウォーキング", route.name)
        assertEquals(3, route.points.size)
        assertEquals(2, route.checkpointsOnRoute.size)
        assertEquals("第1関門", route.checkpointsOnRoute.first().name)
        assertTrue(route.totalM > 0.0)
    }

    @Test
    fun `KML のルート名が無ければ Document 名を使う`() {
        val kml = """
            <kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>Document名</name>
              <Placemark><LineString><coordinates>139.70,35.69 139.71,35.70</coordinates></LineString></Placemark>
            </Document></kml>
        """.trimIndent()
        val route = RouteFileParser.parse(kml.toByteArray(), "fallback")
        assertEquals("Document名", route.name)
    }

    @Test
    fun `KML の gx Track を読める`() {
        val kml = """
            <kml xmlns="http://www.opengis.net/kml/2.2"
                 xmlns:gx="http://www.google.com/kml/ext/2.2">
              <Document><Placemark><name>GPS ログ</name>
                <gx:Track>
                  <when>2026-10-30T08:00:00Z</when>
                  <gx:coord>139.7000 35.6900 0</gx:coord>
                  <when>2026-10-30T08:01:00Z</when>
                  <gx:coord>139.7010 35.6910 0</gx:coord>
                </gx:Track>
              </Placemark></Document>
            </kml>
        """.trimIndent()
        val route = RouteFileParser.parse(kml.toByteArray())
        assertEquals(2, route.points.size)
        assertEquals(35.6910, route.points[1].lat, 1e-9)
        assertEquals(139.7010, route.points[1].lon, 1e-9)
    }

    @Test
    fun `KML の分割 LineString はつながっていれば 1 本になる`() {
        val kml = """
            <kml xmlns="http://www.opengis.net/kml/2.2"><Document>
              <Placemark><LineString><coordinates>139.7000,35.6900 139.7010,35.6910</coordinates></LineString></Placemark>
              <Placemark><LineString><coordinates>139.7010,35.6910 139.7020,35.6920</coordinates></LineString></Placemark>
            </Document></kml>
        """.trimIndent()
        val route = RouteFileParser.parse(kml.toByteArray())
        assertEquals(3, route.points.size)
    }

    @Test
    fun `KML の離れた複数ルートは長い方を選ぶ`() {
        val kml = """
            <kml xmlns="http://www.opengis.net/kml/2.2"><Document>
              <Placemark><name>短い</name><LineString>
                <coordinates>139.7000,35.6900 139.7005,35.6905</coordinates>
              </LineString></Placemark>
              <Placemark><name>長い</name><LineString>
                <coordinates>139.8000,35.7000 139.8100,35.7100 139.8200,35.7200</coordinates>
              </LineString></Placemark>
            </Document></kml>
        """.trimIndent()
        val route = RouteFileParser.parse(kml.toByteArray())
        assertEquals("長い", route.name)
        assertEquals(3, route.points.size)
    }

    @Test
    fun `KML にルートが無ければエラー`() {
        val kml = """
            <kml xmlns="http://www.opengis.net/kml/2.2"><Document>
              <Placemark><name>A</name><Point><coordinates>139.7,35.6</coordinates></Point></Placemark>
            </Document></kml>
        """.trimIndent()
        val e = assertThrows(GpxException::class.java) {
            RouteFileParser.parse(kml.toByteArray())
        }
        assertTrue(e.message!!.contains("LineString"))
    }

    @Test
    fun `KML の DOCTYPE は拒否する`() {
        val kml = """
            <?xml version="1.0"?>
            <!DOCTYPE kml [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>
            <kml xmlns="http://www.opengis.net/kml/2.2"><Document>
              <Placemark><LineString><coordinates>139.7,35.6 139.8,35.7</coordinates></LineString></Placemark>
            </Document></kml>
        """.trimIndent()
        assertThrows(GpxException::class.java) {
            RouteFileParser.parse(kml.toByteArray())
        }
    }

    @Test
    fun `KML の緯度範囲外はエラー`() {
        val kml = """
            <kml xmlns="http://www.opengis.net/kml/2.2"><Document>
              <Placemark><LineString><coordinates>139.7,91.0 139.8,35.7</coordinates></LineString></Placemark>
            </Document></kml>
        """.trimIndent()
        assertThrows(GpxException::class.java) {
            RouteFileParser.parse(kml.toByteArray())
        }
    }

    // ---------- GeoJSON ----------

    @Test
    fun `GeoJSON の Feature LineString を読める`() {
        val json = """
            {"type":"Feature","properties":{"name":"Geo ルート"},
             "geometry":{"type":"LineString","coordinates":[[139.70,35.69],[139.71,35.70],[139.72,35.71]]}}
        """.trimIndent()
        val route = RouteFileParser.parse(json.toByteArray())
        assertEquals("Geo ルート", route.name)
        assertEquals(3, route.points.size)
        assertEquals(35.69, route.points[0].lat, 1e-9)
        assertEquals(139.70, route.points[0].lon, 1e-9)
    }

    @Test
    fun `GeoJSON の FeatureCollection と Point CP を読める`() {
        val json = """
            {"type":"FeatureCollection","features":[
              {"type":"Feature","properties":{"name":"コース"},
               "geometry":{"type":"MultiLineString","coordinates":[
                 [[139.70,35.69],[139.71,35.70]],
                 [[139.71,35.70],[139.72,35.71]]
               ]}},
              {"type":"Feature","properties":{"name":"関門1"},
               "geometry":{"type":"Point","coordinates":[139.71,35.70]}}
            ]}
        """.trimIndent()
        val route = RouteFileParser.parse(json.toByteArray())
        assertEquals("コース", route.name)
        assertEquals(3, route.points.size)
        assertEquals(1, route.checkpointsOnRoute.size)
        assertEquals("関門1", route.checkpointsOnRoute.first().name)
    }

    @Test
    fun `GeoJSON の Polygon は外周をルートにする`() {
        val json = """
            {"type":"Polygon","coordinates":[[[139.70,35.69],[139.71,35.69],[139.71,35.70],[139.70,35.69]]]}
        """.trimIndent()
        val route = RouteFileParser.parse(json.toByteArray())
        assertEquals(4, route.points.size)
    }

    @Test
    fun `GeoJSON が壊れていればエラー`() {
        assertThrows(GpxException::class.java) {
            RouteFileParser.parse("{\"type\":\"LineString\",".toByteArray())
        }
    }

    @Test
    fun `GeoJSON にルートが無ければエラー`() {
        val e = assertThrows(GpxException::class.java) {
            RouteFileParser.parse("{\"type\":\"Point\",\"coordinates\":[139.7,35.6]}".toByteArray())
        }
        assertTrue(e.message!!.contains("LineString"))
    }

    @Test
    fun `GeoJSON の座標順は経度緯度`() {
        val json = """{"type":"LineString","coordinates":[[139.7,35.6],[139.8,35.7]]}"""
        val route = RouteFileParser.parse(json.toByteArray())
        assertEquals(35.6, route.points[0].lat, 1e-9)
        assertEquals(139.7, route.points[0].lon, 1e-9)
    }

    // ---------- 点数の上限 ----------

    @Test
    fun `点数上限を超えたらエラー`() {
        assertThrows(GpxException::class.java) {
            RouteFileParser.checkPointLimit(RouteFileParser.MAX_POINTS, 1)
        }
        RouteFileParser.checkPointLimit(RouteFileParser.MAX_POINTS - 1, 1)
    }
}
