package dev.sasnews.amoledwatch.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

/** T11: アルバムアート → LVGL bin (120x120 RGB565 + 12B ヘッダ + hash)。 */
class MediaArtCodecTest {

    /** 一色のピクセル配列。 */
    private fun solid(w: Int, h: Int, argb: Int): IntArray = IntArray(w * h) { argb }

    @Test
    fun outputIs28812BytesWithLvglHeader() {
        val r = MediaArtCodec.encode(solid(200, 200, 0xFF112233.toInt()), 200, 200)
        assertEquals(MediaArtCodec.BIN_SIZE, r.bin.size)
        // lv_image_header_t LE: magic 0x19, cf 0x12, flags 0, w=120, h=120, stride=240, res 0
        assertEquals(0x19.toByte(), r.bin[0])
        assertEquals(0x12.toByte(), r.bin[1])
        assertEquals(0.toByte(), r.bin[2]); assertEquals(0.toByte(), r.bin[3])
        assertEquals(120.toByte(), r.bin[4]); assertEquals(0.toByte(), r.bin[5])
        assertEquals(120.toByte(), r.bin[6]); assertEquals(0.toByte(), r.bin[7])
        assertEquals(240.toByte(), r.bin[8]); assertEquals(0.toByte(), r.bin[9])
        assertEquals(0.toByte(), r.bin[10]); assertEquals(0.toByte(), r.bin[11])
    }

    @Test
    fun rgb565Conversion() {
        // 白 → 0xFFFF、黒 → 0x0000、赤 → 0xF800
        val white = MediaArtCodec.encode(solid(150, 150, 0xFFFFFFFF.toInt()), 150, 150)
        assertEquals(0xFF.toByte(), white.bin[12]); assertEquals(0xFF.toByte(), white.bin[13])
        val red = MediaArtCodec.encode(solid(150, 150, 0xFFFF0000.toInt()), 150, 150)
        assertEquals(0x00.toByte(), red.bin[12]); assertEquals(0xF8.toByte(), red.bin[13])
        // 緑 0x00FF00 → 0x07E0
        val green = MediaArtCodec.encode(solid(150, 150, 0xFF00FF00.toInt()), 150, 150)
        assertEquals(0xE0.toByte(), green.bin[12]); assertEquals(0x07.toByte(), green.bin[13])
    }

    @Test
    fun centerCropForWideImage() {
        // 横長 200x100: 左右 50px 切り捨て → 中央 100x100 が使われる。
        // 中央だけ赤、左右は青 → 出力は全面赤になるはず。
        val px = IntArray(200 * 100) { i ->
            val x = i % 200
            if (x in 50 until 150) 0xFFFF0000.toInt() else 0xFF0000FF.toInt()
        }
        val r = MediaArtCodec.encode(px, 200, 100)
        // 出力の先頭ピクセルは切り抜き領域内 = 赤
        assertEquals(0x00.toByte(), r.bin[12]); assertEquals(0xF8.toByte(), r.bin[13])
        // 最後尾も同様
        val last = 12 + (120 * 120 - 1) * 2
        assertEquals(0x00.toByte(), r.bin[last]); assertEquals(0xF8.toByte(), r.bin[last + 1])
    }

    @Test
    fun hashIsDeterministicAndContentSensitive() {
        val a = MediaArtCodec.encode(solid(120, 120, 0xFF112233.toInt()), 120, 120)
        val a2 = MediaArtCodec.encode(solid(120, 120, 0xFF112233.toInt()), 120, 120)
        val b = MediaArtCodec.encode(solid(120, 120, 0xFF445566.toInt()), 120, 120)
        assertEquals(a.artId, a2.artId)
        assertNotEquals(a.artId, b.artId)
        assertNotEquals(0L, a.artId)
    }
}
