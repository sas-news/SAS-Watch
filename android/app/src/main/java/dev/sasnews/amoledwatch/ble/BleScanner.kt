package dev.sasnews.amoledwatch.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.ParcelUuid
import android.util.Log
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow

/** サービス UUID で時計だけを拾うスキャナー。 */
class BleScanner(context: Context) {

    data class Found(val device: BluetoothDevice, val name: String?, val rssi: Int)

    private val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager

    /** スキャン中は Flow が生き続ける。collect を止めるとスキャンも止まる。 */
    @SuppressLint("MissingPermission")
    fun scan(): Flow<Found> = callbackFlow {
        val scanner = manager.adapter?.bluetoothLeScanner
        if (scanner == null) {
            close(IllegalStateException("BluetoothLeScanner が取得できません（Bluetooth が OFF?）"))
            return@callbackFlow
        }
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                trySend(Found(result.device, result.scanRecord?.deviceName, result.rssi))
            }

            override fun onBatchScanResults(results: List<ScanResult>) {
                for (r in results) trySend(Found(r.device, r.scanRecord?.deviceName, r.rssi))
            }

            override fun onScanFailed(errorCode: Int) {
                Log.w("BleScanner", "scan failed: $errorCode")
                close(IllegalStateException("スキャン失敗: $errorCode"))
            }
        }
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(BleUuids.SERVICE))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(listOf(filter), settings, cb)
        awaitClose {
            try {
                scanner.stopScan(cb)
            } catch (e: Exception) {
                Log.w("BleScanner", "stopScan", e)
            }
        }
    }
}
