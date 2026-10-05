package dev.sasnews.amoledwatch.protocol

import java.io.ByteArrayOutputStream

/**
 * 最小限の CBOR（RFC 8949）codec。
 * 対応する型: 整数（負数含む）/ byte string / text string / array / map / bool / null。
 * 長は definite-length のみ。float・tag・indefinite-length は未対応。
 * // TODO(hw): 実機で確認 — 時計側が indefinite-length を吐く場合は拡張が必要
 */
sealed interface Cbor {
    data class Cint(val value: Long) : Cbor
    data class Cbytes(val value: ByteArray) : Cbor {
        override fun equals(other: Any?) = other is Cbytes && value.contentEquals(other.value)
        override fun hashCode() = value.contentHashCode()
    }
    data class Ctext(val value: String) : Cbor
    data class Carray(val value: List<Cbor>) : Cbor
    data class Cmap(val value: Map<String, Cbor>) : Cbor
    data class Cbool(val value: Boolean) : Cbor
    data object Cnull : Cbor
}

object CborCodec {

    class DecodeException(message: String, val offset: Int) : Exception("CBOR decode error at $offset: $message")

    // ---------- encode ----------

    fun encode(v: Cbor): ByteArray {
        val out = ByteArrayOutputStream()
        writeValue(out, v)
        return out.toByteArray()
    }

    private fun writeHead(out: ByteArrayOutputStream, major: Int, arg: Long) {
        require(arg >= 0)
        val mt = major shl 5
        when {
            arg < 24 -> out.write(mt or arg.toInt())
            arg < 0x100 -> {
                out.write(mt or 24); out.write(arg.toInt())
            }
            arg < 0x10000 -> {
                out.write(mt or 25); writeUint(out, arg, 2)
            }
            arg < 0x1_0000_0000 -> {
                out.write(mt or 26); writeUint(out, arg, 4)
            }
            else -> {
                out.write(mt or 27); writeUint(out, arg, 8)
            }
        }
    }

    private fun writeUint(out: ByteArrayOutputStream, v: Long, bytes: Int) {
        for (i in bytes - 1 downTo 0) out.write(((v ushr (i * 8)) and 0xFF).toInt())
    }

    private fun writeValue(out: ByteArrayOutputStream, v: Cbor) {
        when (v) {
            is Cbor.Cint -> if (v.value >= 0) writeHead(out, 0, v.value) else writeHead(out, 1, -1 - v.value)
            is Cbor.Cbytes -> {
                writeHead(out, 2, v.value.size.toLong()); out.write(v.value)
            }
            is Cbor.Ctext -> {
                val b = v.value.toByteArray(Charsets.UTF_8)
                writeHead(out, 3, b.size.toLong()); out.write(b)
            }
            is Cbor.Carray -> {
                writeHead(out, 4, v.value.size.toLong())
                v.value.forEach { writeValue(out, it) }
            }
            is Cbor.Cmap -> {
                writeHead(out, 5, v.value.size.toLong())
                v.value.forEach { (k, x) ->
                    val kb = k.toByteArray(Charsets.UTF_8)
                    writeHead(out, 3, kb.size.toLong()); out.write(kb)
                    writeValue(out, x)
                }
            }
            is Cbor.Cbool -> out.write(if (v.value) 0xF5 else 0xF4)
            Cbor.Cnull -> out.write(0xF6)
        }
    }

    // ---------- decode ----------

    fun decode(data: ByteArray): Cbor {
        val r = Reader(data)
        val v = r.value()
        if (r.pos != data.size) throw DecodeException("${data.size - r.pos} trailing bytes", r.pos)
        return v
    }

    /** 先頭の値だけ読む（後続バイトがあってもよい）。読んだバイト数を返す。 */
    fun decodePrefix(data: ByteArray): Pair<Cbor, Int> {
        val r = Reader(data)
        return r.value() to r.pos
    }

    private class Reader(val data: ByteArray) {
        var pos = 0

        private fun byte(): Int {
            if (pos >= data.size) throw DecodeException("unexpected end", pos)
            return data[pos++].toInt() and 0xFF
        }

        private fun readUint(n: Int): Long {
            var v = 0L
            repeat(n) { v = (v shl 8) or byte().toLong() }
            return v
        }

        private fun head(addInfo: Int): Long = when (addInfo) {
            in 0..23 -> addInfo.toLong()
            24 -> readUint(1)
            25 -> readUint(2)
            26 -> readUint(4)
            27 -> readUint(8)
            31 -> throw DecodeException("indefinite length not supported", pos - 1)
            else -> throw DecodeException("reserved additional info $addInfo", pos - 1)
        }

        private fun readBytes(n: Int): ByteArray {
            if (pos + n > data.size) throw DecodeException("unexpected end", pos)
            val b = data.copyOfRange(pos, pos + n)
            pos += n
            return b
        }

        fun value(): Cbor {
            val b = byte()
            val major = b ushr 5
            val add = b and 0x1F
            return when (major) {
                0 -> Cbor.Cint(head(add))
                1 -> Cbor.Cint(-1 - head(add))
                2 -> Cbor.Cbytes(readBytes(head(add).let { if (it > Int.MAX_VALUE) throw DecodeException("too long", pos) else it.toInt() }))
                3 -> Cbor.Ctext(readBytes(head(add).let { if (it > Int.MAX_VALUE) throw DecodeException("too long", pos) else it.toInt() }).toString(Charsets.UTF_8))
                4 -> Cbor.Carray(List(head(add).let { if (it > Int.MAX_VALUE) throw DecodeException("too long", pos) else it.toInt() }) { value() })
                5 -> Cbor.Cmap(buildMap {
                    repeat(head(add).let { if (it > Int.MAX_VALUE) throw DecodeException("too long", pos) else it.toInt() }) {
                        val k = value()
                        if (k !is Cbor.Ctext) throw DecodeException("map key is not text", pos)
                        put(k.value, value())
                    }
                })
                7 -> when (b) {
                    0xF4 -> Cbor.Cbool(false)
                    0xF5 -> Cbor.Cbool(true)
                    0xF6 -> Cbor.Cnull
                    else -> throw DecodeException("unsupported simple/float 0x${b.toString(16)}", pos - 1)
                }
                else -> throw DecodeException("unsupported major type $major", pos - 1)
            }
        }
    }
}

// ----- 取り出し用ヘルパー -----

val Cbor.map: Cbor.Cmap? get() = this as? Cbor.Cmap
val Cbor.text: String? get() = (this as? Cbor.Ctext)?.value
val Cbor.int: Long? get() = (this as? Cbor.Cint)?.value
val Cbor.bool: Boolean? get() = (this as? Cbor.Cbool)?.value

fun Cbor.Cmap.int(key: String, default: Long = 0): Long = (value[key] as? Cbor.Cint)?.value ?: default
fun Cbor.Cmap.text(key: String, default: String = ""): String = (value[key] as? Cbor.Ctext)?.value ?: default
fun Cbor.Cmap.bool(key: String, default: Boolean = false): Boolean = (value[key] as? Cbor.Cbool)?.value ?: default
fun Cbor.Cmap.sub(key: String): Cbor.Cmap? = value[key] as? Cbor.Cmap
fun Cbor.Cmap.textList(key: String): List<String> =
    (value[key] as? Cbor.Carray)?.value?.mapNotNull { (it as? Cbor.Ctext)?.value } ?: emptyList()
