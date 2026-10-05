package dev.sasnews.amoledwatch.ble

import java.util.UUID

/** protocol-v1.md の GATT UUID。 */
object BleUuids {
    val SERVICE: UUID = UUID.fromString("7a1e0001-5d2b-4b8e-9c3f-6f0a11c0a7e1")
    val CTRL: UUID = UUID.fromString("7a1e0002-5d2b-4b8e-9c3f-6f0a11c0a7e1")
    val EVENT: UUID = UUID.fromString("7a1e0003-5d2b-4b8e-9c3f-6f0a11c0a7e1")
    val BULK: UUID = UUID.fromString("7a1e0004-5d2b-4b8e-9c3f-6f0a11c0a7e1")

    /** CCCD（Client Characteristic Configuration Descriptor）は BT 仕様で固定。 */
    val CCC_DESCRIPTOR: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    /** // TODO(hw): 実機で確認 — 実機のサービス広告に含まれる UUID と一致するか */
    const val MTU = 247
}
