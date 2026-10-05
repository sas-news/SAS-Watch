package dev.sasnews.amoledwatch.protocol

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

    private var evtSeq = 0
    private val reqReassembler = Reassembler()

    /** core `settings.hpp` のデフォルト値と同じ。キー順 = 定義順（CBOR 正規形）。 */
    private val settings = LinkedHashMap(SettingsKeys.DEFAULTS)

    private var battery = 87
    private var charging = false
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
                // 実機同様、保存後に EVT を返す
                scheduler.schedule({ emit(Evt.MemoSaved(id)) }, 200, TimeUnit.MILLISECONDS)
                Res.okCbor(Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong()))))
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
            else -> Res.errCbor("unknown_method", "unknown method")
        }
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

    fun close() {
        scheduler.shutdownNow()
    }

    companion object {
        const val FW_VERSION = "0.1.0-fake"
        val CAPS = listOf("timer", "stopwatch", "counter", "memo", "theme")
    }
}
