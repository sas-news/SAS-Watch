package dev.sasnews.amoledwatch.protocol

import java.security.MessageDigest

/** 時計→スマホの BULK 転送の完成品。id = transfer id (BULK_START の id)。 */
data class IncomingBulk(val id: Int, val kind: String, val sha256: ByteArray, val bytes: ByteArray)

/**
 * 時計→スマホの BULK 受信機 (firmware `ble_glue.cpp` の bulk_out と対)。
 * 再構成済みの完全なメッセージ (Reassembler 通過後) を feed すると、
 * 返すべき BULK_ACK と完成した転送を Out で返す。
 * REQ に紐付かない push 転送 (kind="agent" 等) と、`memo.audio.get` の直後に
 * 来る kind="memo" の両方をここで受ける。
 */
class BulkInbound {

    sealed interface Out {
        /** bulk 特性に write-without-response で返すべき BULK_ACK {id,next}。 */
        data class Ack(val id: Int, val next: Long) : Out

        /** 転送完了 (sha256 検証済み)。 */
        data class Completed(val transfer: IncomingBulk) : Out
    }

    private class Transfer(val id: Int, val kind: String, val size: Int, val sha256: ByteArray) {
        val buf = ByteArray(size)
        var next = 0L          // 次に受け取るべき offset
        var sinceAck = 0       // 直近 ACK 以降の受領チャンク数
    }

    private val active = HashMap<Int, Transfer>()

    fun feed(msg: Frame): List<Out> {
        val out = ArrayList<Out>(1)
        when (msg.type) {
            Frame.TYPE_BULK_START -> {
                val m = try {
                    CborCodec.decode(msg.payload) as? Cbor.Cmap
                } catch (e: CborCodec.DecodeException) {
                    null
                } ?: return out
                val id = m.int("id", -1).toInt()
                val size = m.int("size", -1)
                val sha = (m.value["sha256"] as? Cbor.Cbytes)?.value
                val kind = m.text("kind", "")
                if (id !in 0..Frame.MAX_MSG_ID || size <= 0 || size > 16 * 1024 * 1024 ||
                    sha == null || sha.size != 32
                ) {
                    return out
                }
                val t = active[id]
                if (t != null && t.size.toLong() == size && t.sha256.contentEquals(sha)) {
                    // 同じ転送の再送 → 受領位置を返して再開させる
                    out += Out.Ack(id, t.next)
                } else {
                    active[id] = Transfer(id, kind, size.toInt(), sha)
                    // START への ACK は送らない (時計は待たずに CHUNK を出し始める)
                }
            }

            Frame.TYPE_BULK_CHUNK -> {
                val p = msg.payload
                if (p.size < 6) return out
                val id = (p[0].toInt() and 0xFF) or ((p[1].toInt() and 0xFF) shl 8)
                val t = active[id] ?: return out
                val off = (p[2].toLong() and 0xFF) or
                    ((p[3].toLong() and 0xFF) shl 8) or
                    ((p[4].toLong() and 0xFF) shl 16) or
                    ((p[5].toLong() and 0xFF) shl 24)
                val n = p.size - 6
                if (off != t.next || off + n > t.size) {
                    // オフセットずれ/サイズ超過 → 受領位置を教えて再送させる
                    out += Out.Ack(id, t.next)
                    return out
                }
                p.copyInto(t.buf, off.toInt(), 6, 6 + n)
                t.next += n
                if (++t.sinceAck >= BulkCodec.ACK_EVERY) {
                    t.sinceAck = 0
                    out += Out.Ack(id, t.next)
                }
            }

            Frame.TYPE_BULK_END -> {
                val m = try {
                    CborCodec.decode(msg.payload) as? Cbor.Cmap
                } catch (e: CborCodec.DecodeException) {
                    null
                } ?: return out
                val id = m.int("id", -1).toInt()
                val t = active[id] ?: return out
                active.remove(id)
                if (t.next.toInt() == t.size &&
                    MessageDigest.getInstance("SHA-256").digest(t.buf).contentEquals(t.sha256)
                ) {
                    // 検証が通った時だけ最終 ACK{next=size} を返す
                    out += Out.Ack(id, t.next)
                    out += Out.Completed(IncomingBulk(id, t.kind, t.sha256, t.buf))
                }
            }
        }
        return out
    }
}
