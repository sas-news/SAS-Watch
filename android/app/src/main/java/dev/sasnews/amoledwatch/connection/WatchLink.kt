package dev.sasnews.amoledwatch.connection

import dev.sasnews.amoledwatch.protocol.BulkChannel
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.protocol.Res
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/** 時計との接続状態。 */
sealed interface LinkState {
    data object Disconnected : LinkState

    /** attempt = 何回目の接続試行か（再接続中に増える）。 */
    data class Connecting(val attempt: Int) : LinkState
    data object Bonding : LinkState
    data object Connected : LinkState
}

/** 時計との通信路。BLE 実機（BleWatchConnection）か仮想時計（FakeWatchConnection）。 */
interface WatchLink {
    val state: StateFlow<LinkState>

    /** 時計からの EVT。 */
    val events: SharedFlow<Evt>

    /** REQ を送り、msg_id が一致する RES を待つ。タイムアウト 5 秒。 */
    suspend fun request(req: Req): Res

    /** BULK 転送路（bulk 特性）。接続がこの経路を持たないときは null。 */
    val bulk: BulkChannel?

    fun close()
}
