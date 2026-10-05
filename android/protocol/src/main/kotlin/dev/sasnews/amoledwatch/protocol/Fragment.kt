package dev.sasnews.amoledwatch.protocol

import java.io.ByteArrayOutputStream

/**
 * 1メッセージが MTU を超える場合の分割・再構成。
 * 各パケットは独立した Frame（独自の len/crc）で、flags bit0 = 後続あり、
 * seq は 0 からの連番。受信側は seq 順に payload を連結する。
 */
object Fragmenter {

    /** payload を mtuSize 以下の Frame 列に分割する。payload が小さければ 1 フレーム。 */
    fun fragment(type: Int, msgId: Int, payload: ByteArray, mtuSize: Int): List<Frame> {
        val chunk = mtuSize - Frame.HEADER_SIZE - Frame.CRC_SIZE - 3 // ATT notify/write header 3 バイト分 // TODO(hw): 実機で確認
        require(chunk > 0) { "mtu too small: $mtuSize" }
        if (payload.size <= chunk) return listOf(Frame(type, msgId, payload))
        val frames = ArrayList<Frame>((payload.size + chunk - 1) / chunk)
        var off = 0
        var seq = 0
        while (off < payload.size) {
            val n = minOf(chunk, payload.size - off)
            val more = off + n < payload.size
            frames.add(
                Frame(
                    type = type,
                    msgId = msgId,
                    payload = payload.copyOfRange(off, off + n),
                    flags = if (more) Frame.FLAG_MORE else 0,
                    seq = seq,
                ),
            )
            off += n
            seq++
        }
        return frames
    }
}

/**
 * 届いた Frame を順に投げて、メッセージが揃ったら 1 つの Frame（payload 連結済み）を返す。
 * (type, msgId) ごとに独立して再構成する。
 */
class Reassembler {

    private class Partial {
        var nextSeq = 0
        val buf = ByteArrayOutputStream()
    }

    private val partials = HashMap<Long, Partial>()

    private fun key(f: Frame): Long = (f.type.toLong() shl 32) or (f.msgId.toLong() and 0xFFFFFFFFL)

    /** 完成したら Frame、まだなら null。 */
    fun feed(f: Frame): Frame? {
        if (f.seq == 0 && !f.hasMore) return f
        val k = key(f)
        val p = partials.getOrPut(k) { Partial() }
        if (f.seq != p.nextSeq) {
            // 順序違い・欠落 → そのメッセージは捨てる // TODO(hw): 実機で確認（時計側が順序保証するか）
            partials.remove(k)
            throw FrameException("fragment seq mismatch: expected ${p.nextSeq} got ${f.seq}")
        }
        p.buf.write(f.payload)
        p.nextSeq++
        return if (f.hasMore) {
            null
        } else {
            partials.remove(k)
            Frame(type = f.type, msgId = f.msgId, payload = p.buf.toByteArray())
        }
    }
}
