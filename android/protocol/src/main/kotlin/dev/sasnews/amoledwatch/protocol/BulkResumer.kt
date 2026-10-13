package dev.sasnews.amoledwatch.protocol

import java.security.MessageDigest

/**
 * BULK 転送の再開ポリシー (T12, docs/protocol-v1.md §BULK)。
 *
 * 送信途中で切断しても、同じ `transferId`・同じ sha256 で BULK_START を
 * 再送すれば時計は `ACK{next}` に受領済みオフセットを返すので、
 * `BulkSender` はそこから続きを送る (= レジューム)。
 * このクラスは「失敗→再接続待ち→同じ id で再送」の繰り返しだけを持つ
 * 純粋ロジックで、BLE には触れない (テストで差し替え可能)。
 *
 * 終了条件: 成功 / 明示的拒否 (internal 以外の err) /
 * 再接続を `maxAttempts` 回待っても駄目 / `maxTotalMs` 経過。
 */
class BulkResumer(
    private val maxAttempts: Int = 3,
    private val maxTotalMs: Long = 5 * 60_000L,
) {
    /**
     * @param newChannel 再接続ごとに最新の BulkChannel を取り直す。
     * @param awaitReady 切断後の再接続待ち (timeoutMs 内に接続できれば true)。
     */
    suspend fun send(
        newChannel: suspend () -> BulkChannel?,
        awaitReady: suspend (timeoutMs: Long) -> Boolean,
        kind: String,
        data: ByteArray,
        transferId: Int,
        chunk: Int = BulkCodec.DEFAULT_CHUNK,
        now: () -> Long = { System.currentTimeMillis() },
        onAttempt: (attempt: Int, res: Res) -> Unit = { _, _ -> },
        onProgress: (Int, Int) -> Unit = { _, _ -> },
    ): Res {
        val started = now()
        var attempt = 0
        var last: Res = Res.Err("internal", "not attempted")
        while (attempt < maxAttempts && now() - started < maxTotalMs) {
            attempt++
            val ch = newChannel()
            if (ch == null) {
                last = Res.Err("internal", "not connected")
                onAttempt(attempt, last)
                if (!awaitReady(maxTotalMs - (now() - started))) break
                continue
            }
            val res = try {
                BulkSender().send(ch, kind, data, transferId, chunk, onProgress)
            } catch (e: Exception) {
                Res.Err("internal", e.message ?: "send failed")
            }
            onAttempt(attempt, res)
            if (res is Res.Ok) return res
            last = res
            // 時計の明示的な拒否 (bad_request 等) はリトライしない。
            if ((res as? Res.Err)?.code != "internal") return res
            if (attempt >= maxAttempts) break
            if (!awaitReady(maxTotalMs - (now() - started))) break
        }
        return last
    }

    companion object {
        /** content の sha256 先頭 2B LE — 同じデータなら同じ id (= 再開キー)。 */
        fun transferIdOf(data: ByteArray): Int {
            val sha = MessageDigest.getInstance("SHA-256").digest(data)
            return (sha[0].toInt() and 0xFF) or ((sha[1].toInt() and 0xFF) shl 8)
        }
    }
}
