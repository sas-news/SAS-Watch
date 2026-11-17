package dev.sasnews.amoledwatch.agent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/** SSE パーサ (Muse の応答ストリーム) のテスト。 */
class SseParserTest {

    @Test
    fun parsesEventsWithIdAndEventFields() {
        val text = """
            : コメント行は無視
            data: {"event":"initialized","chatId":"c-1"}

            id: 1263
            data: {"event":"text","text":"こん"}

            data: {"event":"turn.done","status":"completed"}
        """.trimIndent() + "\n"
        val events = SseParser().feedAll(text)
        assertEquals(3, events.size)
        assertNull(events[0].id)
        assertEquals("""{"event":"initialized","chatId":"c-1"}""", events[0].data)
        assertEquals("1263", events[1].id)
        assertEquals("""{"event":"text","text":"こん"}""", events[1].data)
        assertEquals("completed", events[2].data.substringAfter("status\":\"").substringBefore('"'))
    }

    @Test
    fun joinsMultilineDataAndHandlesCrlfAndEmptyValue() {
        val p = SseParser()
        assertNull(p.feedLine("event: text"))
        assertNull(p.feedLine("data: line1"))
        assertNull(p.feedLine("data:line2\r"))
        val e = p.feedLine("")!!
        assertEquals("text", e.event)
        assertEquals("line1\nline2", e.data)
    }

    @Test
    fun ignoresEventWithoutDataAndResetsFields() {
        val p = SseParser()
        assertNull(p.feedLine("id: 5"))
        assertNull(p.feedLine("data: a"))
        assertEquals("5", p.feedLine("")!!.id)
        // id は data の無いイベントでは配らず、次のイベントに持ち越さない。
        assertNull(p.feedLine("id: 6"))
        assertNull(p.feedLine(""))
        assertNull(p.feedLine("data: b"))
        val b = p.feedLine("")!!
        assertNull(b.id)
    }

    @Test
    fun finishFlushesLastEventWithoutBlankLine() {
        val p = SseParser()
        assertNull(p.feedLine("data: tail"))
        val e = p.finish()
        assertEquals("tail", e!!.data)
        assertNull(p.finish())
    }

    @Test
    fun eventNameFallsBackToSseEventField() {
        val p = SseParser()
        p.feedLine("event: keepalive")
        p.feedLine("data: {}")
        val e = p.feedLine("")!!
        assertEquals("keepalive", e.event)
    }
}
