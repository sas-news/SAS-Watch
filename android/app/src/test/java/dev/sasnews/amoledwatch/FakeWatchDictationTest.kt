package dev.sasnews.amoledwatch

import dev.sasnews.amoledwatch.connection.FakeWatchConnection
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.InboundBulkEvent
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * FakeWatch 経路 (実機なし) で BULK kind="dictation" が
 * InboundBulkEvent (Start / Audio / End) として届くことを確認する。
 * 実機/ネットワークは使わない。
 */
class FakeWatchDictationTest {

    @Test
    fun fakeDictationArrivesAsInboundEvents() = runBlocking {
        val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
        val link = FakeWatchConnection(scope)
        val events = CopyOnWriteArrayList<InboundBulkEvent>()
        val job = scope.launch { link.inbound.collect { events += it } }

        // 接続待ち (FakeWatchConnection は 500ms 後に Connected になる)
        withTimeoutOrNull(3_000) { link.state.first { it is LinkState.Connected } }

        val id = link.fake.simulateDictation(sec = 1)
        withTimeoutOrNull(3_000) {
            while (events.none { it is InboundBulkEvent.DictationEnd }) delay(10)
        }

        val start = events.filterIsInstance<InboundBulkEvent.DictationStart>().single()
        assertEquals(id, start.id)
        val audio = events.filterIsInstance<InboundBulkEvent.DictationAudio>()
        assertTrue(audio.isNotEmpty())
        val pcmBytes = audio.sumOf { it.pcm.size }
        // 1 秒 × 16kHz × 2B = 32000B (ADPCM の端数で ±4B)
        assertEquals(32_000.0, pcmBytes.toDouble(), 8.0)
        val end = events.filterIsInstance<InboundBulkEvent.DictationEnd>().single()
        assertEquals(id, end.id)

        job.cancel()
        link.close()
        job.join()
    }
}
