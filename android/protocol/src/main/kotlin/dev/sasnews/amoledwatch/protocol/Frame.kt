package dev.sasnews.amoledwatch.protocol

import java.io.ByteArrayOutputStream

/**
 * protocol-v1.md のフレーム。
 *
 * ```
 * | ver:1 | type:1 | flags:1 | seq:1 | msg_id:2 | len:2 | payload | crc16:2 |
 * ```
 * 全フィールド little endian。CRC-16/CCITT-FALSE を offset 0..8+len-1 に掛ける。
 * // TODO(hw): 実機で確認 — CRC のバイトオーダーは LE を採用（doc の "little endian" に合わせた）
 */
data class Frame(
    val type: Int,
    val msgId: Int,
    val payload: ByteArray,
    /** bit0 = 後続フラグメントあり */
    val flags: Int = 0,
    val seq: Int = 0,
) {
    companion object {
        const val VERSION = 1
        const val HEADER_SIZE = 8
        const val CRC_SIZE = 2
        const val FLAG_MORE = 0x01

        const val TYPE_REQ = 0x01
        const val TYPE_RES = 0x02
        const val TYPE_EVT = 0x03
        const val TYPE_BULK_START = 0x10
        const val TYPE_BULK_CHUNK = 0x11
        const val TYPE_BULK_ACK = 0x12
        const val TYPE_BULK_END = 0x13

        const val MAX_MSG_ID = 0xFFFF
        const val MAX_LEN = 0xFFFF

        val TYPE_NAMES = mapOf(
            TYPE_REQ to "REQ", TYPE_RES to "RES", TYPE_EVT to "EVT",
            TYPE_BULK_START to "BULK_START", TYPE_BULK_CHUNK to "BULK_CHUNK",
            TYPE_BULK_ACK to "BULK_ACK", TYPE_BULK_END to "BULK_END",
        )
    }

    val hasMore: Boolean get() = flags and FLAG_MORE != 0
    val typeName: String get() = TYPE_NAMES[type] ?: "0x${type.toString(16)}"

    fun encode(): ByteArray {
        require(payload.size <= MAX_LEN) { "payload too long: ${payload.size}" }
        val out = ByteArrayOutputStream(HEADER_SIZE + payload.size + CRC_SIZE)
        out.write(VERSION)
        out.write(type)
        out.write(flags)
        out.write(seq)
        out.write(msgId and 0xFF)
        out.write((msgId ushr 8) and 0xFF)
        out.write(payload.size and 0xFF)
        out.write((payload.size ushr 8) and 0xFF)
        out.write(payload)
        val body = out.toByteArray()
        val crc = Crc16.compute(body)
        out.write(crc and 0xFF)
        out.write((crc ushr 8) and 0xFF)
        return out.toByteArray()
    }

    override fun equals(other: Any?): Boolean =
        other is Frame && type == other.type && msgId == other.msgId &&
            flags == other.flags && seq == other.seq && payload.contentEquals(other.payload)

    override fun hashCode(): Int =
        31 * (31 * (31 * (31 * type + msgId) + flags) + seq) + payload.contentHashCode()
}

class FrameException(message: String) : Exception(message)

object FrameCodec {

    /** 1フレーム分のバイト列をデコード。CRC 不一致や短すぎる場合は FrameException。 */
    fun decode(data: ByteArray): Frame {
        if (data.size < Frame.HEADER_SIZE + Frame.CRC_SIZE) {
            throw FrameException("frame too short: ${data.size}")
        }
        val ver = data[0].toInt() and 0xFF
        if (ver != Frame.VERSION) throw FrameException("unsupported version $ver")
        val len = (data[6].toInt() and 0xFF) or ((data[7].toInt() and 0xFF) shl 8)
        if (data.size != Frame.HEADER_SIZE + len + Frame.CRC_SIZE) {
            throw FrameException("length mismatch: len=$len actual=${data.size - Frame.HEADER_SIZE - Frame.CRC_SIZE}")
        }
        val expected = (data[data.size - 2].toInt() and 0xFF) or
            ((data[data.size - 1].toInt() and 0xFF) shl 8)
        val actual = Crc16.compute(data, 0, Frame.HEADER_SIZE + len)
        if (expected != actual) {
            throw FrameException("crc mismatch: expected 0x${expected.toString(16)} actual 0x${actual.toString(16)}")
        }
        return Frame(
            type = data[1].toInt() and 0xFF,
            flags = data[2].toInt() and 0xFF,
            seq = data[3].toInt() and 0xFF,
            msgId = (data[4].toInt() and 0xFF) or ((data[5].toInt() and 0xFF) shl 8),
            payload = data.copyOfRange(Frame.HEADER_SIZE, Frame.HEADER_SIZE + len),
        )
    }
}
