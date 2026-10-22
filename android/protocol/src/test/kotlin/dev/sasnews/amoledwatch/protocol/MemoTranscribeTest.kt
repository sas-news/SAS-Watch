package dev.sasnews.amoledwatch.protocol

import org.junit.Assert.assertEquals
import org.junit.Test

class MemoTranscribeTest {
    @Test
    fun transcribeRequestEventParsed() {
        val evt = Evt.fromCbor(
            Cbor.Cmap(
                mapOf(
                    "e" to Cbor.Ctext("memo.transcribe.request"),
                    "d" to Cbor.Cmap(mapOf("id" to Cbor.Cint(7))),
                ),
            ),
        )
        assertEquals(Evt.MemoTranscribeRequest(7), evt)
    }

    @Test
    fun textSetRequestEncodesIdAndText() {
        val req = Req.MemoTextSet(3, "こんにちは")
        assertEquals("memo.text.set", req.method)
        val params = req.params()
        assertEquals(3L, (params.value["id"] as Cbor.Cint).value)
        assertEquals("こんにちは", (params.value["text"] as Cbor.Ctext).value)
    }
}
