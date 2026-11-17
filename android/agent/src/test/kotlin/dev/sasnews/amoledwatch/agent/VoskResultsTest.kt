package dev.sasnews.amoledwatch.agent

import org.junit.Assert.assertEquals
import org.junit.Test

/** Vosk の結果 JSON パース (ネイティブライブラリ不要)。 */
class VoskResultsTest {

    @Test
    fun parsesTextAndPartial() {
        assertEquals("こんにちは、世界", VoskResults.text("""{"text":"こんにちは、世界"}"""))
        assertEquals("こんにち", VoskResults.partial("""{"partial":"こんにち"}"""))
        assertEquals("", VoskResults.text("""{"text":""}"""))
    }

    @Test
    fun toleratesMissingOrBrokenJson() {
        assertEquals("", VoskResults.text(""))
        assertEquals("", VoskResults.text("not json"))
        assertEquals("", VoskResults.text("{}"))
        assertEquals("", VoskResults.partial("""{"partial":null}"""))
    }
}
