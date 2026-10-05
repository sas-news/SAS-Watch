package dev.sasnews.amoledwatch.protocol

import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit
import kotlin.math.max

/**
 * 実機が無い間にアプリを動かすためのインメモリ時計。
 * protocol-v1.md の method を一通り実装する。
 *
 * 使い方：
 * ```
 * val watch = FakeWatch()
 * watch.evtListener = { bytes -> /* EVT frame が届いた */ }
 * val resBytesList = watch.write(frameBytes)
 * ```
 */
class FakeWatch(
    private val scheduler: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor(),
) {

    /** EVT フレーム（エンコード済みバイト列）を受け取るコールバック。 */
    var evtListener: ((ByteArray) -> Unit)? = null

    /** bulk notify（時計→スマホの BULK フレーム）を受け取るコールバック。 */
    var bulkOutListener: ((ByteArray) -> Unit)? = null

    private var evtSeq = 0
    private val reqReassembler = Reassembler()

    /** core `settings.hpp` のデフォルト値と同じ。キー順 = 定義順（CBOR 正規形）。 */
    private val settings = LinkedHashMap(SettingsKeys.DEFAULTS)

    private var battery = 87
    private var charging = false

    /** 今日の歩数（steps.get）。デモ用に初期値を持つ。 */
    var stepsToday = 2450
    private var memoId = 0
    private var timerSeconds = 0
    private var timerTask: java.util.concurrent.ScheduledFuture<*>? = null

    var lastNotification: Triple<String, String, String>? = null
        private set
    var lastMedia: Triple<String, String, Boolean>? = null
        private set

    /** ctrl への write を模倣。Frame バイト列 → RES の Frame バイト列（フラグメント済み）。
     *  REQ がフラグメント途中なら再構成を待って空リストを返す。 */
    fun write(data: ByteArray, mtuSize: Int = 247): List<ByteArray> {
        val frame = FrameCodec.decode(data)
        if (frame.type != Frame.TYPE_REQ) throw FrameException("FakeWatch: not a REQ (type=${frame.typeName})")
        val complete = reqReassembler.feed(frame) ?: return emptyList()
        val res = try {
            handle(parseReq(complete.payload))
        } catch (e: BadReq) {
            Res.errCbor("bad_request", e.message ?: "bad_request")
        }
        return Fragmenter.fragment(Frame.TYPE_RES, complete.msgId, CborCodec.encode(res), mtuSize)
            .map { it.encode() }
    }

    // ---------------- BULK (kind: "theme" 等の受信側。core `BulkReceiver` と同じ規則) ----------------

    private val bulkReassembler = Reassembler()
    private var bulkActive = false
    private var bulkId = 0
    private var bulkKind = ""
    private var bulkSize = 0
    private var bulkExpectedSha = ByteArray(32)
    private var bulkReceived = 0
    private var bulkChunksSinceAck = 0
    private var bulkSha = MessageDigest.getInstance("SHA-256")
    private val bulkBuf = ByteArrayOutputStream()

    /**
     * bulk 特性への write を模倣。BULK_* の Frame バイト列 → 送出すべき Frame 列。
     * ACK は BULK_ACK Frame、BULK_END/BULK_START 失敗時の RES は RES Frame
     * （実機では ctrl notify。呼び出し側が振り分ける）。
     * 途中フラグメントなら再構成を待って空リストを返す。
     */
    fun writeBulk(frameBytes: ByteArray, mtuSize: Int = 247): List<ByteArray> {
        val frame = FrameCodec.decode(frameBytes)
        if (frame.type != Frame.TYPE_BULK_START &&
            frame.type != Frame.TYPE_BULK_CHUNK &&
            frame.type != Frame.TYPE_BULK_END
        ) {
            return emptyList()
        }
        val complete = bulkReassembler.feed(frame) ?: return emptyList()
        val out = ArrayList<ByteArray>()
        when (complete.type) {
            Frame.TYPE_BULK_START -> {
                val s = parseBulkStart(complete.payload)
                    ?: return resFrames(complete.msgId, Res.errCbor("bad_request", "BULK_START"), mtuSize)
                // 同じ転送の再送なら受領位置を返して再開 (core `BulkReceiver::start`)
                if (bulkActive && s.id == bulkId && s.size == bulkSize &&
                    s.sha256.contentEquals(bulkExpectedSha)
                ) {
                    out += ackFrames(s.id, bulkReceived, mtuSize)
                } else {
                    bulkActive = true
                    bulkId = s.id
                    bulkKind = s.kind
                    bulkSize = s.size
                    bulkExpectedSha = s.sha256
                    bulkReceived = 0
                    bulkChunksSinceAck = 0
                    bulkBuf.reset()
                    bulkSha = MessageDigest.getInstance("SHA-256")
                    out += ackFrames(s.id, 0, mtuSize)
                }
            }
            Frame.TYPE_BULK_CHUNK -> {
                val p = complete.payload
                if (!bulkActive || p.size < 6) return emptyList()
                val id = (p[0].toInt() and 0xFF) or ((p[1].toInt() and 0xFF) shl 8)
                if (id != bulkId) return emptyList()
                val offset = (p[2].toLong() and 0xFF) or ((p[3].toLong() and 0xFF) shl 8) or
                    ((p[4].toLong() and 0xFF) shl 16) or ((p[5].toLong() and 0xFF) shl 24)
                val data = p.copyOfRange(6, p.size)
                // オフセット不整合・サイズ超過は現在位置を ACK で知らせて再開させる
                if (offset != bulkReceived.toLong() || bulkReceived + data.size > bulkSize) {
                    out += ackFrames(bulkId, bulkReceived, mtuSize)
                } else {
                    bulkBuf.write(data)
                    bulkSha.update(data)
                    bulkReceived += data.size
                    if (++bulkChunksSinceAck >= BulkCodec.ACK_EVERY) {
                        bulkChunksSinceAck = 0
                        out += ackFrames(bulkId, bulkReceived, mtuSize)
                    }
                }
            }
            Frame.TYPE_BULK_END -> {
                val id = parseBulkEnd(complete.payload)
                if (id == null || !bulkActive || id != bulkId) {
                    return resFrames(complete.msgId, Res.errCbor("bad_request", "BULK_END"), mtuSize)
                }
                val data = bulkBuf.toByteArray()
                val hashOk = bulkReceived == bulkSize &&
                    bulkSha.digest().contentEquals(bulkExpectedSha)
                if (!hashOk) {
                    abortBulk()
                    return resFrames(complete.msgId, Res.errCbor("bad_request", "hash mismatch"), mtuSize)
                }
                // commit: kind="theme" は zip を検査し manifest の id をテーマ設定に入れる
                if (bulkKind == "theme") {
                    val info = try {
                        ThemePackage.inspect(data)
                    } catch (e: ThemePackage.Invalid) {
                        abortBulk()
                        return resFrames(complete.msgId, Res.errCbor("bad_request", e.message ?: "invalid theme"), mtuSize)
                    }
                    settings[SettingsKeys.THEME] = Cbor.Ctext(info.id)
                }
                val wasFirmware = bulkKind == "firmware"
                bulkActive = false
                if (wasFirmware) {
                    // 実機は commit 後にパーティションへ書き込んで再起動する。
                    startFakeOta("BLE")
                }
                return resFrames(complete.msgId, Res.okCbor(), mtuSize)
            }
        }
        return out
    }

    private fun abortBulk() {
        bulkActive = false
        bulkReceived = 0
        bulkBuf.reset()
    }

    private class BulkStartReq(val id: Int, val kind: String, val size: Int, val sha256: ByteArray)

    /** core `bulk_parse_start` と同じ必須チェック: id<=0xFFFF / size / sha256(32B)。kind は省略可。 */
    private fun parseBulkStart(payload: ByteArray): BulkStartReq? {
        val m = try {
            CborCodec.decode(payload) as? Cbor.Cmap
        } catch (e: CborCodec.DecodeException) {
            null
        } ?: return null
        val id = m.int("id", -1)
        val size = m.int("size", -1)
        if (id !in 0..Frame.MAX_MSG_ID || size !in 0..0xFFFFFFFFL) return null
        val sha = (m.value["sha256"] as? Cbor.Cbytes)?.value?.takeIf { it.size == 32 } ?: return null
        val kind = m.text("kind", "")
        if (kind.toByteArray(Charsets.UTF_8).size > BulkCodec.MAX_KIND_LEN) return null
        return BulkStartReq(id.toInt(), kind, size.toInt(), sha)
    }

    private fun parseBulkEnd(payload: ByteArray): Int? {
        val m = try {
            CborCodec.decode(payload) as? Cbor.Cmap
        } catch (e: CborCodec.DecodeException) {
            null
        } ?: return null
        val id = m.int("id", -1)
        return if (id in 0..Frame.MAX_MSG_ID) id.toInt() else null
    }

    private fun ackFrames(id: Int, next: Int, mtuSize: Int): List<ByteArray> {
        val payload = CborCodec.encode(
            Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong()), "next" to Cbor.Cint(next.toLong()))),
        )
        // 実機同様、ACK の msg_id には転送 id を入れる (ble_link.cpp `send_bulk_ack`)
        return Fragmenter.fragment(Frame.TYPE_BULK_ACK, id, payload, mtuSize).map { it.encode() }
    }

    private fun resFrames(msgId: Int, res: Cbor.Cmap, mtuSize: Int): List<ByteArray> =
        Fragmenter.fragment(Frame.TYPE_RES, msgId, CborCodec.encode(res), mtuSize).map { it.encode() }

    /** core の bad_request に対応する内部エラー。RES として返すため FrameException とは分ける。 */
    private class BadReq(msg: String) : Exception(msg)

    fun parseReq(payload: ByteArray): Pair<String, Cbor.Cmap> {
        val m = CborCodec.decode(payload) as? Cbor.Cmap
            ?: throw BadReq("not a map")
        val method = m.text("m") ?: throw BadReq("missing method")
        val p = m.value["p"]?.let {
            it as? Cbor.Cmap ?: throw BadReq("params is not a map")
        } ?: Cbor.Cmap(emptyMap())
        return method to p
    }

    private fun handle(req: Pair<String, Cbor.Cmap>): Cbor.Cmap {
        val (m, p) = req
        return when (m) {
            "hello" -> {
                // core: proto 未指定は bad_request、不一致は unsupported_proto
                val proto = (p.value["proto"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "hello")
                if (proto != Req.PROTO_VERSION) {
                    Res.errCbor("unsupported_proto", "proto $proto は未対応（対応: ${Req.PROTO_VERSION}）。アプリを更新してください")
                } else {
                    Res.okCbor(
                        Cbor.Cmap(
                            mapOf(
                                "proto" to Cbor.Cint(proto.toLong()),
                                "fw" to Cbor.Ctext(FW_VERSION),
                                "caps" to Cbor.Carray(CAPS.map { Cbor.Ctext(it) }),
                            ),
                        ),
                    )
                }
            }
            "time.set" -> {
                // core: epoch 必須。tz_offset_min があれば設定に反映する。
                (p.value["epoch"] as? Cbor.Cint)
                    ?: return Res.errCbor("bad_request", "time.set")
                (p.value["tz_offset_min"] as? Cbor.Cint)?.let {
                    settings[SettingsKeys.TZ_OFFSET_MIN] = it
                }
                Res.okCbor()
            }
            "device.info" -> Res.okCbor(
                Cbor.Cmap(
                    mapOf(
                        "battery" to Cbor.Cint(battery.toLong()),
                        "charging" to Cbor.Cbool(charging),
                        "fw" to Cbor.Ctext(FW_VERSION),
                        "free_heap" to Cbor.Cint(180_000),
                        "free_psram" to Cbor.Cint(7_200_000),
                    ),
                ),
            )
            "settings.get" -> {
                val keysV = p.value["keys"]
                if (keysV == null) {
                    // keys 無し → 全キー（定義順）
                    Res.okCbor(Cbor.Cmap(settings))
                } else {
                    val arr = keysV as? Cbor.Carray
                        ?: return Res.errCbor("bad_request", "settings.get")
                    val picked = LinkedHashMap<String, Cbor>()
                    for (k in arr.value) {
                        // 要求順に返す。text でない要素は bad_request、知らないキーは飛ばす。
                        val name = (k as? Cbor.Ctext)?.value
                            ?: return Res.errCbor("bad_request", "settings.get")
                        settings[name]?.let { picked[name] = it }
                    }
                    Res.okCbor(Cbor.Cmap(picked))
                }
            }
            "settings.set" -> {
                // core: 知らないキーは飛ばし、型が合わない値も飛ばす（RES は ok）。
                for ((k, v) in p.value) {
                    val def = SettingsKeys.DEFAULTS[k] ?: continue
                    val ok = when (def) {
                        // tz_offset_min だけ I32（負数可）。他の int キーは U32。
                        is Cbor.Cint -> v is Cbor.Cint &&
                            (k == SettingsKeys.TZ_OFFSET_MIN || v.value >= 0)
                        is Cbor.Ctext -> v is Cbor.Ctext &&
                            v.value.toByteArray(Charsets.UTF_8).size < 64
                        else -> false
                    }
                    if (ok) settings[k] = v
                }
                Res.okCbor()
            }
            "timer.start" -> {
                // core: seconds は正の int 必須
                val sec = (p.value["seconds"] as? Cbor.Cint)?.value?.toInt()
                if (sec == null || sec <= 0) {
                    return Res.errCbor("bad_request", "timer.start")
                }
                timerSeconds = sec
                timerTask?.cancel(false)
                if (timerSeconds > 0) {
                    timerTask = scheduler.schedule(
                        { emit(Evt.TimerFinished) },
                        timerSeconds.toLong(),
                        TimeUnit.SECONDS,
                    )
                }
                Res.okCbor()
            }
            "timer.stop" -> {
                timerTask?.cancel(false)
                timerTask = null
                Res.okCbor()
            }
            "memo.create" -> {
                // core: text は空でない text 必須
                val text = (p.value["text"] as? Cbor.Ctext)?.value
                if (text.isNullOrEmpty()) {
                    return Res.errCbor("bad_request", "memo.create")
                }
                memoId++
                val id = memoId
                memos[id] = FakeMemo(id, "text", 0, 0, text)
                // 実機同様、保存後に EVT を返す
                scheduler.schedule(
                    { emit(Evt.MemoSaved(id, "text", 0)) },
                    200, TimeUnit.MILLISECONDS,
                )
                Res.okCbor(Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong()))))
            }
            "memo.list" -> {
                // core: i>=0, 1<=n<=16、新しい順
                val i = (p.value["i"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "memo.list")
                val n = (p.value["n"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "memo.list")
                if (i < 0 || n < 1 || n > 16) {
                    return Res.errCbor("bad_request", "memo.list")
                }
                val newest = memos.values.toList().asReversed()
                val slice = newest.drop(i).take(n).map { it.toCbor() }
                Res.okCbor(
                    Cbor.Cmap(
                        mapOf(
                            "total" to Cbor.Cint(newest.size.toLong()),
                            "memos" to Cbor.Carray(slice),
                        ),
                    ),
                )
            }
            "memo.get" -> {
                val id = (p.value["id"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "memo.get")
                val memo = memos[id]
                    ?: return Res.errCbor("not_found", "memo.get")
                Res.okCbor(memo.toCbor(withText = true))
            }
            "memo.delete" -> {
                val id = (p.value["id"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "memo.delete")
                if (memos.remove(id) == null) {
                    return Res.errCbor("not_found", "memo.delete")
                }
                scheduler.schedule(
                    { emit(Evt.MemoDeleted(id)) },
                    200, TimeUnit.MILLISECONDS,
                )
                Res.okCbor()
            }
            "memo.audio.get" -> {
                val id = (p.value["id"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "memo.audio.get")
                val memo = memos[id]
                if (memo == null || memo.kind != "voice" || memo.blob == null) {
                    return Res.errCbor("not_found", "memo.audio.get")
                }
                val sha = java.security.MessageDigest.getInstance("SHA-256")
                    .digest(memo.blob)
                Res.okCbor(
                    Cbor.Cmap(
                        mapOf(
                            "id" to Cbor.Cint(id.toLong()),
                            "size" to Cbor.Cint(memo.blob.size.toLong()),
                            "sha256" to Cbor.Cbytes(sha),
                        ),
                    ),
                )
            }
            "notify.post" -> {
                // core: app/title/body は text 必須
                val app = (p.value["app"] as? Cbor.Ctext)?.value
                val title = (p.value["title"] as? Cbor.Ctext)?.value
                val body = (p.value["body"] as? Cbor.Ctext)?.value
                if (app == null || title == null || body == null) {
                    return Res.errCbor("bad_request", "notify.post")
                }
                lastNotification = Triple(app, title, body)
                Res.okCbor()
            }
            "wifi.set" -> {
                // core: ssid 1-32 / pass 0 または 8-63
                val ssid = (p.value["ssid"] as? Cbor.Ctext)?.value
                val pass = (p.value["pass"] as? Cbor.Ctext)?.value
                if (ssid.isNullOrEmpty() || pass == null ||
                    (pass.isNotEmpty() && pass.length !in 8..63)
                ) {
                    return Res.errCbor("bad_request", "wifi.set")
                }
                wifiSsid = ssid
                Res.okCbor()
            }
            "wifi.status" -> Res.okCbor(
                Cbor.Cmap(
                    mapOf(
                        "configured" to Cbor.Cbool(wifiSsid != null),
                        "ssid" to Cbor.Ctext(wifiSsid ?: ""),
                    ),
                ),
            )
            "ota.start" -> {
                // core: url(text,http(s)) / sha256(bytes32) / version(text)
                val url = (p.value["url"] as? Cbor.Ctext)?.value
                val sha = (p.value["sha256"] as? Cbor.Cbytes)?.value
                val ver = (p.value["version"] as? Cbor.Ctext)?.value
                if (otaBusy()) return Res.errCbor("busy", "ota")
                if (url.isNullOrEmpty() || sha == null || sha.size != 32 ||
                    ver.isNullOrEmpty()
                ) {
                    return Res.errCbor("bad_request", "ota.start")
                }
                startFakeOta(ver)
                Res.okCbor()
            }
            "ota.status" -> Res.okCbor(
                Cbor.Cmap(
                    mapOf(
                        "active" to Cbor.Cbool(otaBusy()),
                        "stage" to Cbor.Ctext(otaStage),
                        "pct" to Cbor.Cint(otaPct.toLong()),
                        "msg" to Cbor.Ctext(otaMsg),
                        "version" to Cbor.Ctext(otaVersion),
                    ),
                ),
            )
            "media.state" -> {
                // core: title/artist は text 必須、playing はあれば bool
                val title = (p.value["title"] as? Cbor.Ctext)?.value
                val artist = (p.value["artist"] as? Cbor.Ctext)?.value
                val playingV = p.value["playing"]
                if (title == null || artist == null ||
                    (playingV != null && playingV !is Cbor.Cbool)
                ) {
                    return Res.errCbor("bad_request", "media.state")
                }
                lastMedia = Triple(title, artist, (playingV as? Cbor.Cbool)?.value ?: false)
                Res.okCbor()
            }
            "alarm.list" -> {
                Res.okCbor(
                    Cbor.Cmap(
                        mapOf(
                            "alarms" to Cbor.Carray(
                                alarms.values.sortedBy { it.id }.map { it.toCbor() },
                            ),
                        ),
                    ),
                )
            }
            "alarm.set" -> {
                // core: id=0 または省略で新規。hour 0-23, min 0-59, dow 0-0x7F, on 省略時 true。
                val id = (p.value["id"] as? Cbor.Cint)?.value?.toInt() ?: 0
                val hour = (p.value["hour"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "alarm.set")
                val min = (p.value["min"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "alarm.set")
                val dow = (p.value["dow"] as? Cbor.Cint)?.value?.toInt() ?: 0
                val on = (p.value["on"] as? Cbor.Cbool)?.value ?: true
                if (hour !in 0..23 || min !in 0..59 || dow < 0 || dow > 0x7F || id < 0) {
                    return Res.errCbor("bad_request", "alarm.set")
                }
                val newId: Int
                if (id == 0) {
                    if (alarms.size >= MAX_ALARMS) {
                        return Res.errCbor("busy", "alarm.set")
                    }
                    newId = ++alarmIdSeq
                    alarms[newId] = FakeAlarm(newId, hour, min, dow, on)
                } else {
                    val cur = alarms[id]
                        ?: return Res.errCbor("not_found", "alarm.set")
                    newId = id
                    alarms[id] = cur.copy(hour = hour, min = min, dow = dow, on = on)
                }
                Res.okCbor(Cbor.Cmap(mapOf("id" to Cbor.Cint(newId.toLong()))))
            }
            "alarm.delete" -> {
                val id = (p.value["id"] as? Cbor.Cint)?.value?.toInt()
                    ?: return Res.errCbor("bad_request", "alarm.delete")
                if (alarms.remove(id) == null) {
                    return Res.errCbor("not_found", "alarm.delete")
                }
                Res.okCbor()
            }
            "steps.get" -> Res.okCbor(
                Cbor.Cmap(
                    mapOf(
                        "steps" to Cbor.Cint(stepsToday.toLong()),
                        "goal" to (settings[SettingsKeys.STEPS_GOAL]
                            ?: SettingsKeys.DEFAULTS.getValue(SettingsKeys.STEPS_GOAL)),
                    ),
                ),
            )
            "agent.reply" -> {
                // core: {id, text}。id は要求と同じ値。
                val id = (p.value["id"] as? Cbor.Cint)?.value?.toInt()
                val text = (p.value["text"] as? Cbor.Ctext)?.value
                if (id == null || id !in 0..Frame.MAX_MSG_ID || text == null) {
                    return Res.errCbor("bad_request", "agent.reply")
                }
                lastAgentReply = id to text
                Res.okCbor()
            }
            else -> Res.errCbor("unknown_method", "unknown method")
        }
    }

    // ---------------- Wi-Fi / OTA (擬似) ----------------

    private var wifiSsid: String? = null
    private var otaStage = "idle"
    private var otaPct = 0
    private var otaMsg = ""
    private var otaVersion = ""

    private fun otaBusy() = otaStage !in listOf("idle", "fail")

    /** HTTPS 開始 or BLE firmware commit 後の進捗を段階的に emit する。 */
    private fun startFakeOta(version: String) {
        otaStage = "download"
        otaPct = 0
        otaVersion = version
        otaMsg = ""
        scheduler.schedule({ emit(Evt.OtaProgress(30, "download")) }, 250, TimeUnit.MILLISECONDS)
        scheduler.schedule({ emit(Evt.OtaProgress(70, "download")) }, 500, TimeUnit.MILLISECONDS)
        scheduler.schedule(
            {
                otaStage = "verify"
                otaPct = 95
                emit(Evt.OtaProgress(95, "verify"))
            },
            750, TimeUnit.MILLISECONDS,
        )
        scheduler.schedule(
            {
                otaStage = "done"
                otaPct = 100
                emit(Evt.OtaResult(true, "reboot"))
                otaStage = "idle"
                otaPct = 0
            },
            1_100, TimeUnit.MILLISECONDS,
        )
    }

    /** 時計 → スマホ の EVT を発火させる（テスト・UI デモ用）。 */
    fun emit(evt: Evt) {
        val frame = Frame(Frame.TYPE_EVT, evtSeq++, CborCodec.encode(evt.toCbor()))
        evtListener?.invoke(frame.encode())
    }

    /** 電池が変わったふりをする。 */
    fun simulateBattery(level: Int, charging: Boolean = this.charging) {
        battery = max(0, level)
        this.charging = charging
        emit(Evt.Battery(battery, charging))
    }

    /** 時計側からのメディア操作を模倣する。 */
    fun simulateMediaCmd(cmd: MediaCmd) = emit(Evt.MediaCommand(cmd))

    /** 時計側で音声メモを録ったふりをする (ADP1 の440Hzトーン)。 */
    fun simulateVoiceMemo(sec: Int = 3) {
        memoId++
        val id = memoId
        val rate = Adpcm.SAMPLE_RATE
        val pcm = ShortArray(rate * sec) { i ->
            (kotlin.math.sin(2.0 * Math.PI * 440.0 * i / rate) * 9000).toInt().toShort()
        }
        val blob = Adpcm.pcmToAdp1(pcm)
        memos[id] = FakeMemo(id, "voice", sec, blob.size, "", blob)
        emit(Evt.MemoSaved(id, "voice", sec))
    }

    /** FakeWatchConnection.fetchBulk が読む音声実体。 */
    fun audioBlobFor(id: Int): ByteArray? = memos[id]?.blob

    private data class FakeAlarm(
        val id: Int,
        val hour: Int,
        val min: Int,
        val dow: Int,
        val on: Boolean,
    ) {
        fun toCbor(): Cbor.Cmap = Cbor.Cmap(
            linkedMapOf(
                "id" to Cbor.Cint(id.toLong()),
                "hour" to Cbor.Cint(hour.toLong()),
                "min" to Cbor.Cint(min.toLong()),
                "dow" to Cbor.Cint(dow.toLong()),
                "on" to Cbor.Cbool(on),
            ),
        )
    }

    private val alarms = LinkedHashMap<Int, FakeAlarm>()
    private var alarmIdSeq = 0

    /** agent.reply REQ で受け取った最後の返答 (id, text)。 */
    var lastAgentReply: Pair<Int, String>? = null
        private set

    private var agentReqId = 0

    /** 時計側からの定型質問を模倣する (EVT agent.request)。返り値は要求 id。 */
    fun simulateAgentRequest(text: String): Int {
        val id = ++agentReqId
        emit(Evt.AgentRequest(id, text))
        return id
    }

    /** 「話しかける」の録音を模倣する (BULK kind="agent_audio" で ADP1 を push)。
     *  bulkOutListener 経由でフレームを出す。返り値は要求 id。 */
    fun simulateAgentAudio(sec: Int = 3, mtuSize: Int = 247): Int {
        val id = ++agentReqId
        val rate = Adpcm.SAMPLE_RATE
        val pcm = ShortArray(rate * sec) { i ->
            (kotlin.math.sin(2.0 * Math.PI * 440.0 * i / rate) * 9000).toInt().toShort()
        }
        pushBulk(id, "agent_audio", Adpcm.pcmToAdp1(pcm), mtuSize)
        return id
    }

    /** data を BULK_START→CHUNK…→END のフレーム列で bulkOutListener に流す。 */
    private fun pushBulk(id: Int, kind: String, data: ByteArray, mtuSize: Int) {
        val listener = bulkOutListener ?: return
        val sha = MessageDigest.getInstance("SHA-256").digest(data)
        val chunk = 512  // core ble_glue の kBulkOutChunk と同じ
        for (f in Fragmenter.fragment(
            Frame.TYPE_BULK_START, id,
            BulkCodec.encodeStart(id, kind, data.size, sha, chunk), mtuSize,
        )) {
            listener(f.encode())
        }
        var off = 0
        while (off < data.size) {
            val n = minOf(chunk, data.size - off)
            for (f in Fragmenter.fragment(
                Frame.TYPE_BULK_CHUNK, id,
                BulkCodec.encodeChunk(id, off.toLong(), data.copyOfRange(off, off + n)),
                mtuSize,
            )) {
                listener(f.encode())
            }
            off += n
        }
        for (f in Fragmenter.fragment(
            Frame.TYPE_BULK_END, id, BulkCodec.encodeEnd(id), mtuSize,
        )) {
            listener(f.encode())
        }
    }

    private class FakeMemo(
        val id: Int,
        val kind: String,
        val sec: Int,
        val size: Int,
        val text: String,
        val blob: ByteArray? = null,
    ) {
        fun toCbor(withText: Boolean = false): Cbor.Cmap {
            val m = linkedMapOf(
                "id" to Cbor.Cint(id.toLong()),
                "kind" to Cbor.Ctext(kind),
                "sec" to Cbor.Cint(sec.toLong()),
                "size" to Cbor.Cint(size.toLong()),
            )
            if (withText) m["text"] = Cbor.Ctext(text)
            return Cbor.Cmap(m)
        }
    }

    private val memos = LinkedHashMap<Int, FakeMemo>()

    fun close() {
        scheduler.shutdownNow()
    }

    companion object {
        const val FW_VERSION = "0.1.0-fake"
        const val MAX_ALARMS = 5
        val CAPS = listOf(
            "timer", "stopwatch", "counter", "memo", "theme", "audio",
            "alarm", "notify", "media", "wifi", "ota", "steps",
            "agent",
        )
    }
}
