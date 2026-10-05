package dev.sasnews.amoledwatch.protocol

import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonNull
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.JsonPrimitive
import java.io.File
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/**
 * docs/protocol-vectors/ の json を読んで encode/decode の一致を検査する。
 * core/tests/test_vectors.cpp と同じケースを実行する。
 *
 * JSON object のキー順がそのまま CBOR map のキー順になる
 * （正規形 = 定義順。Gson の JsonObject は挿入順を保持する）。
 * {"\$bytes": "<hex>"} は byte string。
 */
class VectorsTest {

    private val vectorsDir: File by lazy { locateVectorsDir() }

    private fun locateVectorsDir(): File {
        // Gradle はモジュール dir (android/protocol) でテストを動かす。
        var dir: File? = File(System.getProperty("user.dir")).absoluteFile
        repeat(8) {
            val d = dir ?: return@repeat
            val cand = File(d, "docs/protocol-vectors")
            if (cand.isDirectory) return cand
            dir = d.parentFile
        }
        error("docs/protocol-vectors not found from ${System.getProperty("user.dir")}")
    }

    // ---------- JSON → Cbor ----------

    private fun jsonToCbor(el: JsonElement): Cbor = when {
        el.isJsonNull -> Cbor.Cnull
        el.isJsonPrimitive -> {
            val p = el.asJsonPrimitive
            when {
                p.isBoolean -> Cbor.Cbool(p.asBoolean)
                p.isNumber -> Cbor.Cint(p.asLong)
                else -> Cbor.Ctext(p.asString)
            }
        }
        el.isJsonArray -> Cbor.Carray(el.asJsonArray.map { jsonToCbor(it) })
        el.isJsonObject -> {
            val obj = el.asJsonObject
            val entries = obj.entrySet().toList()
            if (entries.size == 1 && entries[0].key == "\$bytes") {
                Cbor.Cbytes(hexToBytes(entries[0].value.asString))
            } else {
                Cbor.Cmap(
                    buildMap {
                        entries.forEach { (k, v) -> put(k, jsonToCbor(v)) }
                    },
                )
            }
        }
        else -> error("unsupported json: $el")
    }

    // ---------- Cbor → 比較用モデル ----------

    private fun cborToModel(v: Cbor): Any? = when (v) {
        is Cbor.Cint -> v.value
        is Cbor.Cbytes -> v.value.toList().map { it.toInt() and 0xFF }
        is Cbor.Ctext -> v.value
        is Cbor.Carray -> v.value.map { cborToModel(it) }
        is Cbor.Cmap -> v.value.map { (k, x) -> k to cborToModel(x) }
        is Cbor.Cbool -> v.value
        Cbor.Cnull -> null
    }

    private fun jsonToModel(el: JsonElement): Any? = when {
        el.isJsonNull -> null
        el.isJsonPrimitive -> {
            val p = el.asJsonPrimitive
            when {
                p.isBoolean -> p.asBoolean
                p.isNumber -> p.asLong
                else -> p.asString
            }
        }
        el.isJsonArray -> el.asJsonArray.map { jsonToModel(it) }
        el.isJsonObject -> {
            val obj = el.asJsonObject
            val entries = obj.entrySet().toList()
            if (entries.size == 1 && entries[0].key == "\$bytes") {
                hexToBytes(entries[0].value.asString).toList().map { it.toInt() and 0xFF }
            } else {
                entries.map { (k, v) -> k to jsonToModel(v) }
            }
        }
        else -> error("unsupported json: $el")
    }

    // ---------- hex ----------

    private fun hexToBytes(s: String): ByteArray {
        val t = s.filter { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
        require(t.length % 2 == 0) { "odd hex" }
        return ByteArray(t.length / 2) { t.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
    }

    private fun ByteArray.hex() = joinToString("") { "%02x".format(it) }

    // ---------- frame ----------

    private val types = mapOf(
        "REQ" to Frame.TYPE_REQ, "RES" to Frame.TYPE_RES, "EVT" to Frame.TYPE_EVT,
        "BULK_START" to Frame.TYPE_BULK_START, "BULK_CHUNK" to Frame.TYPE_BULK_CHUNK,
        "BULK_ACK" to Frame.TYPE_BULK_ACK, "BULK_END" to Frame.TYPE_BULK_END,
    )

    private fun runFrameCase(c: JsonObject) {
        val name = c["name"].asString
        c["expect_error"]?.let {
            // デコード専用の失敗ケース
            val raw = hexToBytes(c["raw_hex"].asString)
            try {
                FrameCodec.decode(raw)
                fail("$name: expected decode error '${it.asString}'")
            } catch (e: FrameException) {
                val want = when (it.asString) {
                    "too_short" -> "short"
                    "bad_version" -> "version"
                    "bad_length" -> "length"
                    "bad_crc" -> "crc"
                    else -> it.asString
                }
                assertTrue(
                    "$name: error message should contain '$want' (got: ${e.message})",
                    e.message!!.contains(want),
                )
            }
            return
        }

        val type = types[c["type"].asString] ?: error("$name: unknown type")
        val msgId = c["msg_id"].asInt
        val mtu = c["mtu"]?.asInt ?: 247
        val payload = hexToBytes(c["payload_hex"].asString)
        val wantFrames = c["frames"].asJsonArray.map { it.asString }

        // encode: payload → fragment → frames hex と一致
        val got = Fragmenter.fragment(type, msgId, payload, mtu).map { it.encode().hex() }
        assertEquals("$name: frame count", wantFrames.size, got.size)
        wantFrames.forEachIndexed { i, want ->
            assertEquals("$name frame[$i]", want, got[i])
        }

        // decode: frames hex → decode + reassemble → payload と一致
        val re = Reassembler()
        var done: Frame? = null
        wantFrames.forEachIndexed { i, fh ->
            val f = FrameCodec.decode(hexToBytes(fh))
            assertEquals("$name frame[$i] type", type, f.type)
            assertEquals("$name frame[$i] msg_id", msgId, f.msgId)
            assertEquals("$name frame[$i] seq", i, f.seq)
            done = re.feed(f) ?: done
        }
        assertNotNull("$name: reassemble", done)
        assertArrayEquals("$name: payload", payload, done!!.payload)
    }

    private fun cases(file: String): JsonArray {
        val root = JsonParser.parseReader(File(vectorsDir, file).reader()).asJsonObject
        return root["cases"].asJsonArray
    }

    // ---------- tests ----------

    @Test
    fun cborVectors() {
        for (el in cases("cbor.json")) {
            val c = el.asJsonObject
            val name = c["name"].asString
            val value = c["value"]
            val want = c["hex"].asString

            // encode
            assertEquals("$name: encode", want, CborCodec.encode(jsonToCbor(value)).hex())
            // decode
            val decoded = CborCodec.decode(hexToBytes(want))
            assertEquals("$name: decode", jsonToModel(value), cborToModel(decoded))
        }
    }

    @Test
    fun frameVectors() = cases("frame.json").forEach { runFrameCase(it.asJsonObject) }

    @Test
    fun messageVectors() = cases("messages.json").forEach { runFrameCase(it.asJsonObject) }
}
