package dev.sasnews.amoledwatch.protocol

/**
 * CRC-16/CCITT-FALSE（多項式 0x1021、初期値 0xFFFF、反射なし、xorout 0x0000）。
 * "123456789" → 0x29B1 になるもの。
 */
object Crc16 {
    private val TABLE = IntArray(256) { i ->
        var crc = i shl 8
        repeat(8) {
            crc = if (crc and 0x8000 != 0) (crc shl 1) xor 0x1021 else crc shl 1
            crc = crc and 0xFFFF
        }
        crc
    }

    fun compute(data: ByteArray, offset: Int = 0, length: Int = data.size - offset): Int {
        var crc = 0xFFFF
        for (i in offset until offset + length) {
            crc = ((crc shl 8) xor TABLE[((crc ushr 8) xor (data[i].toInt() and 0xFF)) and 0xFF]) and 0xFFFF
        }
        return crc
    }
}
