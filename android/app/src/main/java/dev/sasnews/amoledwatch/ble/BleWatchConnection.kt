package dev.sasnews.amoledwatch.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.connection.WatchLink
import dev.sasnews.amoledwatch.protocol.BulkAck
import dev.sasnews.amoledwatch.protocol.BulkChannel
import dev.sasnews.amoledwatch.protocol.BulkCodec
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.CborCodec
import dev.sasnews.amoledwatch.protocol.Evt
import dev.sasnews.amoledwatch.protocol.Frame
import dev.sasnews.amoledwatch.protocol.FrameCodec
import dev.sasnews.amoledwatch.protocol.FrameException
import dev.sasnews.amoledwatch.protocol.Fragmenter
import dev.sasnews.amoledwatch.protocol.Reassembler
import dev.sasnews.amoledwatch.protocol.Req
import dev.sasnews.amoledwatch.protocol.Res
import dev.sasnews.amoledwatch.protocol.int
import dev.sasnews.amoledwatch.protocol.toCbor
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull

/**
 * protocol-v1.md の BLE 実装。
 * - REQ は ctrl へ書き込み、同じ msg_id の RES を notify で待つ（5 秒タイムアウト）。
 * - 切断されたら指数バックオフ（1s → 最大30s）で再接続し続ける。
 * - 未ボンドなら createBond()（時計側は Numeric Comparison で6桁を表示する想定）。
 * // TODO(hw): 実機で確認 — ボンディングの流れ（実機が要求する pairing 方式）
 */
@SuppressLint("MissingPermission") // 呼び出し側（スキャン/接続ボタン）で権限を確認済み
class BleWatchConnection(
    private val context: Context,
    private val device: BluetoothDevice,
    private val scope: CoroutineScope,
) : WatchLink {

    companion object {
        private const val TAG = "BleWatchConnection"
        private const val REQ_TIMEOUT_MS = 5_000L
        private const val OP_TIMEOUT_MS = 10_000L
        private const val BULK_OP_TIMEOUT_MS = 10_000L
        private const val BOND_TIMEOUT_MS = 60_000L
        private const val BACKOFF_MAX_MS = 30_000L
    }

    private val _state = MutableStateFlow<LinkState>(LinkState.Connecting(1))
    override val state: StateFlow<LinkState> = _state

    private val _events = MutableSharedFlow<Evt>(extraBufferCapacity = 64)
    override val events: SharedFlow<Evt> = _events

    /** BULK_* フレーム（将来の Asset/OTA 転送用に流しておく）。 */
    private val _bulkFrames = MutableSharedFlow<Frame>(extraBufferCapacity = 64)
    val bulkFrames: SharedFlow<Frame> = _bulkFrames

    /** 届いた BULK_ACK（`{id,next}`）。BleBulkChannel が受け取る。 */
    private val bulkAcks = Channel<BulkAck>(Channel.UNLIMITED)
    override val bulk: BulkChannel = BleBulkChannel()

    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var mtu: Int = 23
    @Volatile private var closed = false

    private var ctrlChar: BluetoothGattCharacteristic? = null
    private var eventChar: BluetoothGattCharacteristic? = null
    private var bulkChar: BluetoothGattCharacteristic? = null

    private val resReassembler = Reassembler()
    private val evtReassembler = Reassembler()
    private val bulkReassembler = Reassembler()

    private val msgIds = AtomicInteger(1)
    private val pending = ConcurrentHashMap<Int, CompletableDeferred<Res>>()

    // ---- 直列化された GATT 操作の完了シグナル ----
    private val opMutex = Mutex()
    @Volatile private var connectSignal = CompletableDeferred<Boolean>()
    @Volatile private var servicesSignal = CompletableDeferred<Boolean>()
    @Volatile private var mtuSignal = CompletableDeferred<Boolean>()
    @Volatile private var descSignal = CompletableDeferred<Boolean>()
    @Volatile private var writeSignal = CompletableDeferred<Boolean>()
    @Volatile private var disconnectSignal = CompletableDeferred<Boolean>()
    @Volatile private var bondSignal = CompletableDeferred<Boolean>()

    private var bondReceiver: BroadcastReceiver? = null

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    if (!connectSignal.isCompleted) connectSignal.complete(true)
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    Log.i(TAG, "disconnected (status=$status)")
                    if (!connectSignal.isCompleted) connectSignal.complete(false)
                    if (!disconnectSignal.isCompleted) disconnectSignal.complete(true)
                    failAllPending("disconnected")
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            servicesSignal.complete(status == BluetoothGatt.GATT_SUCCESS)
        }

        override fun onMtuChanged(g: BluetoothGatt, negotiated: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) mtu = negotiated
            mtuSignal.complete(status == BluetoothGatt.GATT_SUCCESS)
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            if (!descSignal.isCompleted) descSignal.complete(status == BluetoothGatt.GATT_SUCCESS)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (!writeSignal.isCompleted) writeSignal.complete(status == BluetoothGatt.GATT_SUCCESS)
        }

        // API <33
        @Deprecated("deprecated in API 33", ReplaceWith(""))
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            onNotify(c.uuid, c.value)
        }

        // API 33+
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            onNotify(c.uuid, value)
        }
    }

    private val job = scope.launch { run() }

    // ---------------- 接続ループ（指数バックオフ再接続） ----------------

    private suspend fun run() {
        var attempt = 0
        while (!closed) {
            attempt++
            _state.value = LinkState.Connecting(attempt)
            val connected = try {
                connectOnce()
            } catch (e: Exception) {
                Log.w(TAG, "connectOnce failed", e)
                false
            }
            if (closed) break
            if (connected) {
                _state.value = LinkState.Connected
                disconnectSignal.await()
                closeGatt()
            }
            if (closed) break
            val wait = backoff(attempt)
            Log.i(TAG, "retry in ${wait}ms (attempt $attempt)")
            delay(wait)
        }
        closeGatt()
        _state.value = LinkState.Disconnected
    }

    private fun backoff(attempt: Int): Long {
        val shift = (attempt - 1).coerceAtMost(5)
        return minOf(BACKOFF_MAX_MS, 1_000L shl shift)
    }

    /** 1回の接続試行。notify 購読まで済んだら true。 */
    private suspend fun connectOnce(): Boolean {
        if (device.bondState != BluetoothDevice.BOND_BONDED && !bond()) return false

        connectSignal = CompletableDeferred()
        servicesSignal = CompletableDeferred()
        mtuSignal = CompletableDeferred()
        disconnectSignal = CompletableDeferred()

        val g = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
        if (g == null) return false
        gatt = g

        if (!awaitSignal(connectSignal, "connect")) { closeGatt(); return false }
        if (!g.discoverServices() || !awaitSignal(servicesSignal, "services")) { closeGatt(); return false }

        val service = g.getService(BleUuids.SERVICE)
        ctrlChar = service?.getCharacteristic(BleUuids.CTRL)
        eventChar = service?.getCharacteristic(BleUuids.EVENT)
        bulkChar = service?.getCharacteristic(BleUuids.BULK)
        if (ctrlChar == null || eventChar == null) {
            Log.w(TAG, "service/chars not found: service=$service")
            closeGatt(); return false
        }

        if (!g.requestMtu(BleUuids.MTU) || !awaitSignal(mtuSignal, "mtu")) { closeGatt(); return false }
        Log.i(TAG, "MTU=$mtu")

        // notify 購読（ctrl / event / bulk を順に）
        for (c in listOfNotNull(ctrlChar, eventChar, bulkChar)) {
            if (!enableNotify(g, c)) { closeGatt(); return false }
        }
        return true
    }

    private suspend fun awaitSignal(signal: CompletableDeferred<Boolean>, what: String): Boolean =
        withTimeoutOrNull(OP_TIMEOUT_MS) { signal.await() } ?: run {
            Log.w(TAG, "$what timed out")
            false
        }

    private suspend fun enableNotify(g: BluetoothGatt, c: BluetoothGattCharacteristic): Boolean {
        if (!g.setCharacteristicNotification(c, true)) return false
        val d = c.getDescriptor(BleUuids.CCC_DESCRIPTOR) ?: return false
        descSignal = CompletableDeferred()
        val ok = writeDescriptor(g, d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
        if (!ok) return false
        return awaitSignal(descSignal, "cccd ${c.uuid}")
    }

    private suspend fun bond(): Boolean {
        _state.value = LinkState.Bonding
        bondSignal = CompletableDeferred()
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                val dev = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                }
                if (dev?.address != device.address) return
                when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.ERROR)) {
                    BluetoothDevice.BOND_BONDED -> bondSignal.complete(true)
                    BluetoothDevice.BOND_NONE -> bondSignal.complete(false)
                }
            }
        }
        bondReceiver = receiver
        ContextCompat.registerReceiver(
            context, receiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_EXPORTED,
        )
        try {
            if (!device.createBond()) return false
            return withTimeoutOrNull(BOND_TIMEOUT_MS) { bondSignal.await() } ?: false
        } finally {
            try {
                context.unregisterReceiver(receiver)
            } catch (_: IllegalArgumentException) {
            }
            bondReceiver = null
        }
    }

    // ---------------- REQ/RES ----------------

    private fun nextMsgId(): Int {
        val v = msgIds.getAndIncrement()
        if (v >= Frame.MAX_MSG_ID) msgIds.set(1)
        return v and Frame.MAX_MSG_ID
    }

    override suspend fun request(req: Req): Res {
        if (closed) return Res.Err("internal", "closed")
        val g = gatt ?: return Res.Err("internal", "not connected")
        val ctrl = ctrlChar ?: return Res.Err("internal", "ctrl characteristic not found")
        val id = nextMsgId()
        val deferred = CompletableDeferred<Res>()
        pending[id] = deferred
        try {
            val frames = Fragmenter.fragment(Frame.TYPE_REQ, id, CborCodec.encode(req.toCbor()), mtu)
            for (f in frames) {
                if (!writeRaw(g, ctrl, f.encode(), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)) {
                    pending.remove(id)
                    return Res.Err("internal", "write failed")
                }
            }
            return withTimeout(REQ_TIMEOUT_MS) { deferred.await() }
        } catch (e: TimeoutCancellationException) {
            pending.remove(id)
            return Res.Err("internal", "timeout (5s)")
        } catch (e: Exception) {
            pending.remove(id)
            return Res.Err("internal", e.message ?: "request failed")
        }
    }

    /** キャラクタリスティック write（GATT の完了コールバックを待つ直列化）。 */
    private suspend fun writeRaw(
        g: BluetoothGatt,
        c: BluetoothGattCharacteristic,
        bytes: ByteArray,
        writeType: Int,
    ): Boolean {
        opMutex.withLock {
            writeSignal = CompletableDeferred()
            val ok = writeCharacteristic(g, c, bytes, writeType)
            if (!ok) return false
            return awaitSignal(writeSignal, "write")
        }
    }

    private fun writeCharacteristic(
        g: BluetoothGatt,
        c: BluetoothGattCharacteristic,
        value: ByteArray,
        writeType: Int,
    ): Boolean =
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, value, writeType) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                c.writeType = writeType
                c.value = value
                g.writeCharacteristic(c)
            }
        }

    private fun writeDescriptor(g: BluetoothGatt, d: BluetoothGattDescriptor, value: ByteArray): Boolean =
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(d, value) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                d.value = value
                g.writeDescriptor(d)
            }
        }

    // ---------------- notify 受信 ----------------

    private fun onNotify(uuid: java.util.UUID, value: ByteArray) {
        val frame = try {
            FrameCodec.decode(value)
        } catch (e: FrameException) {
            Log.w(TAG, "bad frame on $uuid: ${e.message}")
            return
        }
        when (uuid) {
            BleUuids.CTRL -> handleRes(frame)
            BleUuids.EVENT -> handleEvt(frame)
            BleUuids.BULK -> handleBulk(frame)
        }
    }

    private fun handleRes(fragment: Frame) {
        val complete = try {
            resReassembler.feed(fragment) ?: return
        } catch (e: FrameException) {
            Log.w(TAG, "RES reassembly: ${e.message}")
            return
        }
        val res = try {
            Res.fromCbor(CborCodec.decode(complete.payload))
        } catch (e: Exception) {
            Res.Err("internal", "bad RES cbor: ${e.message}")
        }
        pending.remove(complete.msgId)?.complete(res)
    }

    private fun handleEvt(fragment: Frame) {
        val complete = try {
            evtReassembler.feed(fragment) ?: return
        } catch (e: FrameException) {
            Log.w(TAG, "EVT reassembly: ${e.message}")
            return
        }
        try {
            Evt.fromCbor(CborCodec.decode(complete.payload))?.let { _events.tryEmit(it) }
        } catch (e: Exception) {
            Log.w(TAG, "bad EVT cbor: ${e.message}")
        }
    }

    private fun handleBulk(fragment: Frame) {
        val complete = try {
            bulkReassembler.feed(fragment) ?: return
        } catch (e: FrameException) {
            Log.w(TAG, "BULK reassembly: ${e.message}")
            return
        }
        if (complete.type == Frame.TYPE_BULK_ACK) {
            BulkCodec.decodeAck(complete.payload)?.let { bulkAcks.trySend(it) }
        }
        _bulkFrames.tryEmit(complete)
    }

    // ---------------- BULK 受信 (Watch → Phone, kind="memo") ----------------

    /** 受信側の ACK (BULK_ACK{id,next}) を bulk char へ write-without-response。 */
    private suspend fun sendBulkAck(tid: Int, next: Long): Boolean {
        val g = gatt ?: return false
        val bulk = bulkChar ?: return false
        val payload = CborCodec.encode(
            Cbor.Cmap(
                mapOf(
                    "id" to Cbor.Cint(tid.toLong()),
                    "next" to Cbor.Cint(next),
                ),
            ),
        )
        return try {
            for (f in Fragmenter.fragment(Frame.TYPE_BULK_ACK, tid, payload, mtu)) {
                if (!writeRaw(g, bulk, f.encode(),
                        BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)
                ) return false
            }
            true
        } catch (e: Exception) {
            Log.w(TAG, "bulk ack write: ${e.message}")
            false
        }
    }

    override suspend fun fetchBulk(id: Int, sha256: ByteArray, timeoutMs: Long): ByteArray? {
        val tid = id and Frame.MAX_MSG_ID
        if (bulkChar == null) return null
        val done = CompletableDeferred<ByteArray?>()
        val job = scope.launch {
            var buf = ByteArray(0)
            var expectNext = 0L
            var sinceAck = 0
            bulkFrames.collect { f ->
                if (done.isCompleted) return@collect
                when (f.type) {
                    Frame.TYPE_BULK_START -> {
                        val m = CborCodec.decode(f.payload) as? Cbor.Cmap
                        val sid = m?.int("id")?.toInt()
                        val size = m?.int("size")?.toInt()
                        if (m != null && sid != null && size != null &&
                            (sid and Frame.MAX_MSG_ID) == tid && size > 0
                        ) {
                            buf = ByteArray(size)
                            expectNext = 0
                            sinceAck = 0
                        }
                    }
                    Frame.TYPE_BULK_CHUNK -> {
                        val p = f.payload
                        if (buf.isEmpty() || p.size < 6) return@collect
                        val cid = (p[0].toInt() and 0xFF) or
                            ((p[1].toInt() and 0xFF) shl 8)
                        if (cid != tid) return@collect
                        val off = (p[2].toLong() and 0xFF) or
                            ((p[3].toLong() and 0xFF) shl 8) or
                            ((p[4].toLong() and 0xFF) shl 16) or
                            ((p[5].toLong() and 0xFF) shl 24)
                        val n = p.size - 6
                        if (off != expectNext || off + n > buf.size) {
                            // ずれた → 受信済みの続き位置を教えて再送してもらう。
                            sendBulkAck(tid, expectNext)
                            return@collect
                        }
                        p.copyInto(buf, off.toInt(), 6, 6 + n)
                        expectNext += n
                        if (++sinceAck >= 8) {
                            sinceAck = 0
                            sendBulkAck(tid, expectNext)
                        }
                    }
                    Frame.TYPE_BULK_END -> {
                        val m = CborCodec.decode(f.payload) as? Cbor.Cmap
                        if (m == null || (m.int("id").toInt() and Frame.MAX_MSG_ID) != tid ||
                            buf.isEmpty()
                        ) return@collect
                        val ok = expectNext.toInt() == buf.size &&
                            java.security.MessageDigest.getInstance("SHA-256")
                                .digest(buf).contentEquals(sha256)
                        // 完了の合図: 検証が通った時だけ最終 ACK{next=size} を返す。
                        if (ok) sendBulkAck(tid, expectNext)
                        done.complete(if (ok) buf else null)
                    }
                }
            }
        }
        val res = try {
            withTimeoutOrNull(timeoutMs) { done.await() }
        } finally {
            job.cancel()
        }
        return res
    }

    private fun failAllPending(reason: String) {
        val it = pending.entries.iterator()
        while (it.hasNext()) {
            it.next().value.complete(Res.Err("internal", reason))
            it.remove()
        }
    }

    // ---------------- BULK ----------------

    /**
     * bulk 特性 (WRITE_NO_RESPONSE + notify) 経由の BulkChannel。
     * BULK_ACK は bulk notify の `{id,next}`。BULK_END / BULK_START 失敗の RES は
     * ctrl notify に同じ msg_id で返るので、既存の `pending` 機構で待つ。
     * gatt/bulkChar は接続ごとに張り替わるので、呼び出し時に読む。
     */
    @OptIn(ExperimentalCoroutinesApi::class)
    inner class BleBulkChannel : BulkChannel {

        override suspend fun start(id: Int, kind: String, size: Int, sha256: ByteArray, chunk: Int): BulkAck? {
            val g = gatt ?: return null
            val c = bulkChar ?: return null
            val msgId = nextMsgId()
            val deferred = CompletableDeferred<Res>()
            pending[msgId] = deferred
            try {
                val payload = BulkCodec.encodeStart(id, kind, size, sha256, chunk)
                for (f in Fragmenter.fragment(Frame.TYPE_BULK_START, msgId, payload, mtu)) {
                    if (!writeRaw(g, c, f.encode(), BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)) {
                        return null
                    }
                }
                return withTimeoutOrNull(BULK_OP_TIMEOUT_MS) {
                    // ACK (bulk notify) と RES err (ctrl notify) の先着。
                    val waiter = async {
                        var a: BulkAck
                        do {
                            a = bulkAcks.receive()
                        } while (a.id != id)
                        a
                    }
                    val winner = select<BulkAck?> {
                        waiter.onAwait { it }
                        deferred.onAwait { null }
                    }
                    waiter.cancel()
                    winner
                }
            } finally {
                pending.remove(msgId)
            }
        }

        override suspend fun chunk(id: Int, offset: Long, data: ByteArray) {
            val g = gatt ?: throw IOException("not connected")
            val c = bulkChar ?: throw IOException("bulk characteristic not found")
            val payload = BulkCodec.encodeChunk(id, offset, data)
            // msg_id に転送 id を入れる（実機は CHUNK の msg_id を見ないが、ログ用に追跡しやすい）
            for (f in Fragmenter.fragment(Frame.TYPE_BULK_CHUNK, id, payload, mtu)) {
                if (!writeRaw(g, c, f.encode(), BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)) {
                    throw IOException("bulk chunk write failed")
                }
            }
        }

        override suspend fun nextAck(timeoutMs: Long): BulkAck? =
            if (timeoutMs <= 0) {
                bulkAcks.tryReceive().getOrNull()
            } else {
                withTimeoutOrNull(timeoutMs) { bulkAcks.receive() }
            }

        override suspend fun end(id: Int): Res {
            val g = gatt ?: return Res.Err("internal", "not connected")
            val c = bulkChar ?: return Res.Err("internal", "bulk characteristic not found")
            val msgId = nextMsgId()
            val deferred = CompletableDeferred<Res>()
            pending[msgId] = deferred
            try {
                for (f in Fragmenter.fragment(Frame.TYPE_BULK_END, msgId, BulkCodec.encodeEnd(id), mtu)) {
                    if (!writeRaw(g, c, f.encode(), BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE)) {
                        return Res.Err("internal", "write failed")
                    }
                }
                return withTimeout(BULK_OP_TIMEOUT_MS) { deferred.await() }
            } catch (e: TimeoutCancellationException) {
                return Res.Err("internal", "timeout")
            } finally {
                pending.remove(msgId)
            }
        }
    }

    // ---------------- 後片付け ----------------

    private fun closeGatt() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
    }

    override fun close() {
        if (closed) return
        closed = true
        disconnectSignal.complete(true)
        connectSignal.complete(false)
        failAllPending("closed")
        closeGatt()
        job.cancel()
    }
}
