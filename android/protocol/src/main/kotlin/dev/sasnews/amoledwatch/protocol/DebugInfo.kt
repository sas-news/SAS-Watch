package dev.sasnews.amoledwatch.protocol

/**
 * `debug.info` (T13) の応答。取れない項目は省略されるので nullable。
 * heap_int/psram は空き、*_min は最小空き (bytes)。
 */
data class DebugInfo(
    val heapInt: Long? = null,
    val heapIntMin: Long? = null,
    val psram: Long? = null,
    val psramMin: Long? = null,
    val uptimeS: Long? = null,
    val resetReason: Long? = null,
    val fw: String? = null,
    val screen: String? = null,
    val battery: Long? = null,
    val battMv: Long? = null,
    val vbusMv: Long? = null,
    val bleRssi: Long? = null,
    val bleItvlMs: Long? = null,
) {
    companion object {
        fun fromCbor(v: Cbor): DebugInfo {
            val m = v as? Cbor.Cmap ?: return DebugInfo()
            return DebugInfo(
                heapInt = m.intOrNull("heap_int"),
                heapIntMin = m.intOrNull("heap_int_min"),
                psram = m.intOrNull("psram"),
                psramMin = m.intOrNull("psram_min"),
                uptimeS = m.intOrNull("uptime_s"),
                resetReason = m.intOrNull("reset_reason"),
                fw = m.textOrNull("fw"),
                screen = m.textOrNull("screen"),
                battery = m.intOrNull("battery"),
                battMv = m.intOrNull("batt_mv"),
                vbusMv = m.intOrNull("vbus_mv"),
                bleRssi = m.intOrNull("ble_rssi"),
                bleItvlMs = m.intOrNull("ble_itvl_ms"),
            )
        }

        private fun Cbor.Cmap.intOrNull(k: String): Long? =
            (value[k] as? Cbor.Cint)?.value

        private fun Cbor.Cmap.textOrNull(k: String): String? =
            (value[k] as? Cbor.Ctext)?.value
    }
}
