package dev.sasnews.amoledwatch.connection

import dev.sasnews.amoledwatch.protocol.BulkChannel
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.IncomingBulk
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

    /**
     * 時計→スマホに届いた BULK 転送 (sha256 検証済み)。
     * `memo.audio.get` の応答 (kind="memo") と REQ に紐付かない push
     * (kind="agent_audio" 等) の両方が流れる。
     */
    val incomingBulk: SharedFlow<IncomingBulk>

    /**
     * 時計→スマホの BULK 転送 (kind="memo") を受け取る。
     * `memo.audio.get` の RES 直後に呼ぶ。転送 id = id & 0xFFFF。
     * sha256 は RES で受け取った期待値。失敗・不一致・タイムアウトなら null。
     */
    suspend fun fetchBulk(id: Int, sha256: ByteArray, timeoutMs: Long = 30_000): ByteArray?

    fun close()
}
