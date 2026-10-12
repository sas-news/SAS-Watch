package dev.sasnews.amoledwatch

import dev.sasnews.amoledwatch.media.MediaStateCoalescer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class MediaStateCoalescerTest {

    private fun st(title: String, playing: Boolean = true) =
        MediaStateCoalescer.State(title, "artist", playing)

    @Test
    fun `identical state is suppressed`() {
        val c = MediaStateCoalescer()
        assertEquals(
            MediaStateCoalescer.Decision.SendNow(st("a")),
            c.offer(0, st("a")),
        )
        assertEquals(MediaStateCoalescer.Decision.Suppress, c.offer(10_000, st("a")))
    }

    @Test
    fun `changed state within interval is throttled with trailing`() {
        val c = MediaStateCoalescer()
        assertEquals(MediaStateCoalescer.Decision.SendNow(st("a")), c.offer(0, st("a")))
        val d = c.offer(100, st("b"))
        assertEquals(
            MediaStateCoalescer.Decision.Pending(st("b"), sendAfterMs = 500),
            d,
        )
        // 保留がある間のさらに新しい状態は pending を差し替える
        assertEquals(
            MediaStateCoalescer.Decision.Pending(st("c"), sendAfterMs = 500),
            c.offer(200, st("c")),
        )
        // 期限前の flush はまだ送らない
        assertNull(c.flush(300))
        // 期限到達で最新の pending が trailing 送信される (最終変化は取りこぼさない)
        assertEquals(st("c"), c.flush(500))
    }

    @Test
    fun `state repeated while pending is suppressed`() {
        val c = MediaStateCoalescer()
        c.offer(0, st("a"))
        assertEquals(
            MediaStateCoalescer.Decision.Pending(st("b"), sendAfterMs = 500),
            c.offer(100, st("b")),
        )
        assertEquals(MediaStateCoalescer.Decision.Suppress, c.offer(200, st("b")))
    }

    @Test
    fun `change after interval sends immediately`() {
        val c = MediaStateCoalescer()
        c.offer(0, st("a"))
        assertEquals(MediaStateCoalescer.Decision.SendNow(st("b")), c.offer(600, st("b")))
    }

    @Test
    fun `playing toggle is a real change`() {
        val c = MediaStateCoalescer()
        assertEquals(
            MediaStateCoalescer.Decision.SendNow(st("a", playing = true)),
            c.offer(0, st("a", playing = true)),
        )
        assertEquals(
            MediaStateCoalescer.Decision.SendNow(st("a", playing = false)),
            c.offer(600, st("a", playing = false)),
        )
    }

    @Test
    fun `reset clears dedupe`() {
        val c = MediaStateCoalescer()
        c.offer(0, st("a"))
        c.reset()
        assertEquals(MediaStateCoalescer.Decision.SendNow(st("a")), c.offer(0, st("a")))
    }
}
