package dev.sasnews.amoledwatch.walk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * GpxParser のテスト。距離の期待値は地球半径 6371008.8 m の haversine から計算する。
 * 赤道沿い 1° = 111194.9266 m。
 */
class GpxParserTest {
    private fun gpx(body: String): ByteArray = (
        """<?xml version="1.0" encoding="UTF-8"?>""" +
            """<gpx version="1.1" creator="test" xmlns="http://www.topografix.com/GPX/1/1">""" +
            body +
            "</gpx>"
        ).toByteArray(Charsets.UTF_8)

    @Test
    fun `trk points and named wpt become route and checkpoints`() {
        val bytes = gpx(
            """
            <trk><name>○○街道 20km</name><trkseg>
              <trkpt lat="0.0" lon="0.0"><ele>10</ele></trkpt>
              <trkpt lat="0.0" lon="0.001"/>
              <trkpt lat="0.0" lon="0.0015"/>
              <trkpt lat="0.0" lon="0.003"/>
            </trkseg></trk>
            <wpt lat="0.0" lon="0.0015"><name>第1関門</name></wpt>
            <wpt lat="0.0" lon="0.002"><desc>名前なし</desc></wpt>
            """.trimIndent(),
        )
        val route = GpxParser.parse(bytes, "fallback.gpx")
        assertEquals("○○街道 20km", route.name)
        assertEquals(4, route.points.size)
        assertEquals(1, route.checkpointsOnRoute.size)
        assertEquals("第1関門", route.checkpointsOnRoute[0].name)
        assertEquals(166.792, route.checkpointsOnRoute[0].distanceM, 0.01)
        assertEquals(333.585, route.totalM, 0.01)
    }

    @Test
    fun `rte points are used when trk has no points`() {
        val bytes = gpx(
            """
            <rte><name>周回コース</name>
              <rtept lat="35.68" lon="139.76"/>
              <rtept lat="35.681" lon="139.761"/>
              <rtept lat="35.682" lon="139.76"/>
            </rte>
            """.trimIndent(),
        )
        val route = GpxParser.parse(bytes, "fallback.gpx")
        assertEquals("周回コース", route.name)
        assertEquals(3, route.points.size)
    }

    @Test
    fun `trk name wins over rte name and trk points win over rte points`() {
        val bytes = gpx(
            """
            <trk><name>トラック名</name><trkseg>
              <trkpt lat="0.0" lon="0.0"/><trkpt lat="0.0" lon="0.001"/>
            </trkseg></trk>
            <rte><name>ルート名</name>
              <rtept lat="1.0" lon="1.0"/><rtept lat="1.0" lon="1.001"/>
            </rte>
            """.trimIndent(),
        )
        val route = GpxParser.parse(bytes, "fallback.gpx")
        assertEquals("トラック名", route.name)
        assertEquals(0.0, route.points[0].lat, 0.0)
    }

    @Test
    fun `name falls back to file name`() {
        val bytes = gpx(
            """
            <trk><trkseg>
              <trkpt lat="0.0" lon="0.0"/><trkpt lat="0.0" lon="0.001"/>
            </trkseg></trk>
            """.trimIndent(),
        )
        assertEquals("大会コース.gpx", GpxParser.parse(bytes, "大会コース.gpx").name)
        assertEquals("ルート", GpxParser.parse(bytes).name)
    }

    @Test
    fun `wpt without name is ignored`() {
        val bytes = gpx(
            """
            <trk><trkseg>
              <trkpt lat="0.0" lon="0.0"/><trkpt lat="0.0" lon="0.001"/>
            </trkseg></trk>
            <wpt lat="0.0" lon="0.0005"><name>  </name></wpt>
            """.trimIndent(),
        )
        assertTrue(GpxParser.parse(bytes).checkpointsOnRoute.isEmpty())
    }

    @Test
    fun `too many points is rejected`() {
        val points = buildString {
            repeat(GpxParser.MAX_POINTS + 1) { i ->
                append("""<trkpt lat="0.0" lon="${(i % 1000) * 0.000001}"/>""")
            }
        }
        val bytes = gpx("<trk><trkseg>$points</trkseg></trk>")
        val e = runCatching { GpxParser.parse(bytes) }.exceptionOrNull()
        assertTrue(e is GpxException)
        assertTrue(e!!.message!!.contains("点が多すぎ"))
    }

    @Test
    fun `oversized file is rejected before parsing`() {
        val bytes = ByteArray(GpxParser.MAX_BYTES + 1)
        val e = runCatching { GpxParser.parse(bytes) }.exceptionOrNull()
        assertTrue(e is GpxException)
        assertTrue(e!!.message!!.contains("大きすぎ"))
    }

    @Test
    fun `malformed xml is rejected`() {
        val bytes = gpx("<trk><trkseg><trkpt lat=\"0\" lon=\"0\"></trkseg></trk>")
        assertTrue(runCatching { GpxParser.parse(bytes) }.exceptionOrNull() is GpxException)
    }

    @Test
    fun `doctype is rejected (xxe)`() {
        val bytes = (
            """<?xml version="1.0"?>""" +
                """<!DOCTYPE gpx [<!ENTITY xxe SYSTEM "file:///etc/passwd">]>""" +
                """<gpx><trk><trkseg>""" +
                """<trkpt lat="0.0" lon="0.0"/><trkpt lat="0.0" lon="0.001"/>""" +
                "</trkseg></trk></gpx>"
            ).toByteArray(Charsets.UTF_8)
        val e = runCatching { GpxParser.parse(bytes) }.exceptionOrNull()
        assertTrue(e is GpxException)
    }

    @Test
    fun `invalid latitude is rejected`() {
        val bytes = gpx(
            """
            <trk><trkseg>
              <trkpt lat="95.0" lon="0.0"/><trkpt lat="0.0" lon="0.001"/>
            </trkseg></trk>
            """.trimIndent(),
        )
        val e = runCatching { GpxParser.parse(bytes) }.exceptionOrNull()
        assertTrue(e is GpxException)
        assertTrue(e!!.message!!.contains("緯度"))
    }

    @Test
    fun `non gpx root is rejected`() {
        val bytes = "<kml><Placemark/></kml>".toByteArray()
        val e = runCatching { GpxParser.parse(bytes) }.exceptionOrNull()
        assertTrue(e is GpxException)
        assertTrue(e!!.message!!.contains("GPX"))
    }

    @Test
    fun `single point route is rejected`() {
        val bytes = gpx("""<trk><trkseg><trkpt lat="0.0" lon="0.0"/></trkseg></trk>""")
        assertTrue(runCatching { GpxParser.parse(bytes) }.exceptionOrNull() is GpxException)
    }
}
