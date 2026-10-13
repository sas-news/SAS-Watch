package dev.sasnews.amoledwatch.protocol

import java.security.MessageDigest

/**
 * アルバムアートの LVGL bin 変換 (T11)。プラットフォーム非依存:
 * ARGB ピクセル配列 → 中央切り抜き + 120x120 縮小 → RGB565。
 *
 * wire フォーマット (docs/protocol-v1.md §media.state):
 *   12B ヘッダ = lv_image_header_t LE
 *     {magic:u8=0x19, cf:u8=0x12(RGB565), flags:u16=0,
 *      w:u16=120, h:u16=120, stride:u16=240, reserved:u16=0}
 *   + ピクセル 28,800B (RGB565、左上→右下) = 計 28,812B
 */
object MediaArtCodec {
    const val SIZE = 120
    const val BIN_SIZE = 12 + SIZE * SIZE * 2
    private const val MAGIC = 0x19
    private const val CF_RGB565 = 0x12

    data class Result(
        val bin: ByteArray,  // LVGL bin 全体 (ヘッダ含む)
        val artId: Long,     // sha256 先頭4B LE (media.state の "art" 値)
    )

    /**
     * ARGB8888 ピクセル (w×h、row-major) から LVGL bin を作る。
     * 中央正方形切り抜き → 最近傍で 120x120 に縮小 → RGB565。
     */
    fun encode(pixels: IntArray, w: Int, h: Int): Result {
        require(pixels.size == w * h) { "pixels size != w*h" }
        val out = ByteArray(BIN_SIZE)
        // ---- ヘッダ (lv_image_header_t, little endian) ----
        out[0] = MAGIC.toByte()
        out[1] = CF_RGB565.toByte()
        // flags:u16 = 0 (out[2..3])
        putLe16(out, 4, SIZE)
        putLe16(out, 6, SIZE)
        putLe16(out, 8, SIZE * 2)  // stride (bytes)
        // reserved_2:u16 = 0 (out[10..11])

        // ---- 中央切り抜き + 最近傍縮小 ----
        val side = minOf(w, h)
        val ox = (w - side) / 2
        val oy = (h - side) / 2
        for (y in 0 until SIZE) {
            val sy = oy + y * side / SIZE
            for (x in 0 until SIZE) {
                val sx = ox + x * side / SIZE
                val c = pixels[sy * w + sx]
                val r = (c ushr 16) and 0xFF
                val g = (c ushr 8) and 0xFF
                val b = c and 0xFF
                val v = ((r ushr 3) shl 11) or ((g ushr 2) shl 5) or (b ushr 3)
                val o = 12 + (y * SIZE + x) * 2
                out[o] = (v and 0xFF).toByte()
                out[o + 1] = (v ushr 8).toByte()
            }
        }
        return Result(bin = out, artId = artIdOf(out))
    }

    /** art id = sha256 先頭 4 バイト LE。 */
    fun artIdOf(bin: ByteArray): Long {
        val d = MessageDigest.getInstance("SHA-256").digest(bin)
        return (d[0].toLong() and 0xFF) or
            ((d[1].toLong() and 0xFF) shl 8) or
            ((d[2].toLong() and 0xFF) shl 16) or
            ((d[3].toLong() and 0xFF) shl 24)
    }

    private fun putLe16(out: ByteArray, off: Int, v: Int) {
        out[off] = (v and 0xFF).toByte()
        out[off + 1] = ((v ushr 8) and 0xFF).toByte()
    }
}
