package dev.sasnews.amoledwatch.protocol

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class DebugInfoTest {
    private fun cmap(vararg pairs: Pair<String, Cbor>) = Cbor.Cmap(mapOf(*pairs))

    @Test
    fun parsesAllFields() {
        val v = DebugInfo.fromCbor(
            cmap(
                "heap_int" to Cbor.Cint(140000), "psram" to Cbor.Cint(5600000),
                "uptime_s" to Cbor.Cint(3600), "reset_reason" to Cbor.Cint(1),
                "fw" to Cbor.Ctext("1.0.0"), "screen" to Cbor.Ctext("timer"),
                "battery" to Cbor.Cint(87), "batt_mv" to Cbor.Cint(4120),
                "vbus_mv" to Cbor.Cint(5010),
                "ble_rssi" to Cbor.Cint(-58), "ble_itvl_ms" to Cbor.Cint(45),
            ),
        )
        assertEquals(140000L, v.heapInt)
        assertEquals(5600000L, v.psram)
        assertEquals(3600L, v.uptimeS)
        assertEquals("1.0.0", v.fw)
        assertEquals("timer", v.screen)
        assertEquals(-58L, v.bleRssi)
        assertEquals(45L, v.bleItvlMs)
    }

    @Test
    fun missingOptionalFieldsAreNull() {
        val v = DebugInfo.fromCbor(cmap("fw" to Cbor.Ctext("x")))
        assertEquals("x", v.fw)
        assertNull(v.bleRssi); assertNull(v.battMv); assertNull(v.psram)
    }
}

class SettingsEditorTest {
    @Test
    fun kindForKnownKeys() {
        assertEquals(SettingsEditor.Kind.Choice,
            SettingsEditor.kindOf("face", Cbor.Ctext("bold")))
        assertEquals(SettingsEditor.Kind.Int,
            SettingsEditor.kindOf("brightness", Cbor.Cint(50)))
        assertEquals(SettingsEditor.Kind.Bool,
            SettingsEditor.kindOf("raise_to_wake", Cbor.Cint(1)))
        assertEquals(SettingsEditor.Kind.Text,
            SettingsEditor.kindOf("agent.q1", Cbor.Ctext("hi")))
        // 未知キーは型で決まる
        assertEquals(SettingsEditor.Kind.Int,
            SettingsEditor.kindOf("custom.key", Cbor.Cint(3)))
    }

    @Test
    fun choicesForKnownKeys() {
        assertEquals(FaceNames.ALL, SettingsEditor.choices("face"))
        assertEquals(ActionNames.ALL, SettingsEditor.choices("button.boot.short"))
        assertEquals(null, SettingsEditor.choices("brightness"))
    }
}
