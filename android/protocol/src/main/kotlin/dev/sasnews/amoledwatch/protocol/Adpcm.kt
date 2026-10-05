package dev.sasnews.amoledwatch.protocol

/**
 * ADP1 (独自ヘッダ付き IMA-ADPCM, 16kHz mono 4bit) のデコーダ。
 * core `adpcm.cpp` と同じ IMA-ADPCM (89 段ステップ) で、
 * 時計から取った .adp を PCM 化し WAV に詰め直す。
 *
 * ADP1 ヘッダ (16 bytes, little endian):
 *   0..3  "ADP1"
 *   4     version = 1
 *   5     channels = 1
 *   6     bits/sample = 4
 *   7     reserved = 0
 *   8..11 sample rate = 16000
 *   12..15 サンプル数
 */
object Adpcm {

    const val ADP1_HEADER_SIZE = 16
    const val SAMPLE_RATE = 16000

    /** パース結果。body は data 内の符号部オフセット以降を指す。 */
    data class Adp1(val samples: Int, val body: ByteArray)

    private fun u32(b: ByteArray, off: Int): Long =
        (b[off].toLong() and 0xFF) or
            ((b[off + 1].toLong() and 0xFF) shl 8) or
            ((b[off + 2].toLong() and 0xFF) shl 16) or
            ((b[off + 3].toLong() and 0xFF) shl 24)

    /** ADP1 をパースして符号部を返す。形式が違えば null。 */
    fun parse(data: ByteArray): Adp1? {
        if (data.size < ADP1_HEADER_SIZE) return null
        if (!(data[0] == 'A'.code.toByte() && data[1] == 'D'.code.toByte() &&
                data[2] == 'P'.code.toByte() && data[3] == '1'.code.toByte())
        ) return null
        if (data[4].toInt() != 1 || data[5].toInt() != 1 || data[6].toInt() != 4) return null
        if (u32(data, 8) != SAMPLE_RATE.toLong()) return null
        return Adp1(
            samples = u32(data, 12).toInt(),
            body = data.copyOfRange(ADP1_HEADER_SIZE, data.size),
        )
    }

    private val stepTable = intArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    )

    private val indexTable = intArrayOf(
        -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
    )

    /** IMA-ADPCM デコーダ (1byte=2サンプル, 下位ニブルが先)。 */
    class Decoder {
        private var pred = 0
        private var index = 0

        private fun step(nib: Int): Short {
            val step = stepTable[index]
            var vpdiff = step shr 3
            if (nib and 4 != 0) vpdiff += step
            if (nib and 2 != 0) vpdiff += step shr 1
            if (nib and 1 != 0) vpdiff += step shr 2
            pred += if (nib and 8 != 0) -vpdiff else vpdiff
            pred = pred.coerceIn(-32768, 32767)
            index = (index + indexTable[nib and 0x0F]).coerceIn(0, 88)
            return pred.toShort()
        }

        fun decode(data: ByteArray): ShortArray {
            val out = ShortArray(data.size * 2)
            var w = 0
            for (b in data) {
                val v = b.toInt() and 0xFF
                out[w++] = step(v and 0x0F)
                out[w++] = step(v shr 4)
            }
            return out
        }
    }

    /** IMA-ADPCM エンコーダ (デモ/テストで時計と同じ形式を作る用)。 */
    class Encoder {
        private var pred = 0
        private var index = 0
        private var pending = 0
        private var haveHalf = false
        private val out = java.io.ByteArrayOutputStream()

        private fun step(s: Short): Int {
            var diff = s.toInt() - pred
            var nib = 0
            if (diff < 0) { nib = 8; diff = -diff }
            var step = stepTable[index]
            var vpdiff = step shr 3
            if (diff >= step) { nib = nib or 4; diff -= step; vpdiff += step }
            step = step shr 1
            if (diff >= step) { nib = nib or 2; diff -= step; vpdiff += step }
            step = step shr 1
            if (diff >= step) { nib = nib or 1; vpdiff += step }
            pred += if (nib and 8 != 0) -vpdiff else vpdiff
            pred = pred.coerceIn(-32768, 32767)
            index = (index + indexTable[nib]).coerceIn(0, 88)
            return nib
        }

        fun encode(pcm: ShortArray) {
            for (s in pcm) {
                val nib = step(s)
                if (!haveHalf) {
                    pending = nib
                    haveHalf = true
                } else {
                    out.write(pending or (nib shl 4))
                    haveHalf = false
                }
            }
        }

        fun finish(): ByteArray {
            if (haveHalf) {
                out.write(pending)
                haveHalf = false
            }
            return out.toByteArray()
        }
    }

    /** ADP1 ヘッダを書く (body バイト列は呼び出し側で続けて書く)。 */
    fun adp1Header(samples: Int): ByteArray {
        val h = ByteArray(ADP1_HEADER_SIZE)
        "ADP1".toByteArray().copyInto(h, 0)
        h[4] = 1; h[5] = 1; h[6] = 4; h[7] = 0
        for (i in 0..3) {
            h[8 + i] = (SAMPLE_RATE ushr (8 * i) and 0xFF).toByte()
            h[12 + i] = (samples ushr (8 * i) and 0xFF).toByte()
        }
        return h
    }

    /** PCM → ADP1 バイト列 (FakeWatch のデモ音声用)。 */
    fun pcmToAdp1(pcm: ShortArray): ByteArray {
        val enc = Encoder()
        enc.encode(pcm)
        return adp1Header(pcm.size) + enc.finish()
    }

    /** ADP1 バイト列 → 16bit PCM (失敗なら null)。 */
    fun decodePcm(data: ByteArray): ShortArray? {
        val adp1 = parse(data) ?: return null
        val pcm = Decoder().decode(adp1.body)
        // ヘッダのサンプル数に合わせて切り詰める (末尾の半端を捨てる)。
        return if (adp1.samples < pcm.size) pcm.copyOf(adp1.samples) else pcm
    }

    /** 16bit PCM → WAV (PCM16, 16kHz mono) バイト列。 */
    fun toWav(pcm: ShortArray): ByteArray {
        val dataLen = pcm.size * 2
        val out = ByteArray(44 + dataLen)
        fun putI(off: Int, v: Long) {
            out[off] = (v and 0xFF).toByte()
            out[off + 1] = (v shr 8 and 0xFF).toByte()
            out[off + 2] = (v shr 16 and 0xFF).toByte()
            out[off + 3] = (v shr 24 and 0xFF).toByte()
        }
        fun putS(off: Int, v: Int) {
            out[off] = (v and 0xFF).toByte()
            out[off + 1] = (v shr 8 and 0xFF).toByte()
        }
        "RIFF".toByteArray().copyInto(out, 0)
        putI(4, (36 + dataLen).toLong())
        "WAVE".toByteArray().copyInto(out, 8)
        "fmt ".toByteArray().copyInto(out, 12)
        putI(16, 16)           // fmt chunk size
        putS(20, 1)            // PCM
        putS(22, 1)            // mono
        putI(24, SAMPLE_RATE.toLong())
        putI(28, (SAMPLE_RATE * 2).toLong())  // byte rate
        putS(32, 2)            // block align
        putS(34, 16)           // bits/sample
        "data".toByteArray().copyInto(out, 36)
        putI(40, dataLen.toLong())
        for (i in pcm.indices) {
            val v = pcm[i].toInt() and 0xFFFF
            out[44 + i * 2] = (v and 0xFF).toByte()
            out[44 + i * 2 + 1] = (v shr 8 and 0xFF).toByte()
        }
        return out
    }

    /** ADP1 → WAV 一発変換 (失敗なら null)。 */
    fun adp1ToWav(data: ByteArray): ByteArray? = decodePcm(data)?.let { toWav(it) }
}
