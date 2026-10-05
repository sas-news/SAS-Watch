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

    private val settings = linkedMapOf<String, Cbor>(
        SettingsKeys.BRIGHTNESS to Cbor.Cint(70),
        SettingsKeys.DIM_AFTER_S to Cbor.Cint(8),
        SettingsKeys.SCREEN_OFF_AFTER_S to Cbor.Cint(12),
        SettingsKeys.BUTTON_BOOT_SHORT to Cbor.Ctext("timer.toggle"),
        SettingsKeys.BUTTON_BOOT_LONG to Cbor.Ctext("nav.dev"),
        SettingsKeys.BUTTON_BOOT_DOUBLE to Cbor.Ctext("memo.record"),
        SettingsKeys.BUTTON_PWR_SHORT to Cbor.Ctext("nav.back"),
        SettingsKeys.BUTTON_PWR_LONG to Cbor.Ctext("power.menu"),
        SettingsKeys.THEME to Cbor.Ctext("standard"),
    )

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
        val res = handle(parseReq(complete.payload))
        return Fragmenter.fragment(Frame.TYPE_RES, complete.msgId, CborCodec.encode(res), mtuSize)
            .map { it.encode() }
    }

    fun parseReq(payload: ByteArray): Pair<String, Cbor.Cmap> {
        val m = CborCodec.decode(payload) as? Cbor.Cmap
            ?: throw FrameException("REQ payload is not a map")
        return (m.text("m") ?: throw FrameException("REQ missing \"m\"")) to
            (m.sub("p") ?: Cbor.Cmap(emptyMap()))
    }

    private fun handle(req: Pair<String, Cbor.Cmap>): Cbor.Cmap {
        val (m, p) = req
        return when (m) {
            "hello" -> {
                val proto = p.int("proto").toInt()
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
            "time.set" -> Res.okCbor()
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
                val keys = p.textList("keys")
                val picked = if (keys.isEmpty()) settings else settings.filterKeys { it in keys }
                Res.okCbor(Cbor.Cmap(picked))
            }
            "settings.set" -> {
                settings.putAll(p.value)
                Res.okCbor()
            }
            "timer.start" -> {
                timerSeconds = p.int("seconds").toInt()
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
                memoId++
                val id = memoId
                // 実機同様、保存後に EVT を返す
                scheduler.schedule({ emit(Evt.MemoSaved(id)) }, 200, TimeUnit.MILLISECONDS)
                Res.okCbor(Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong()))))
            }
            "notify.post" -> {
                lastNotification = Triple(p.text("app"), p.text("title"), p.text("body"))
                Res.okCbor()
            }
            "media.state" -> {
                lastMedia = Triple(p.text("title"), p.text("artist"), p.bool("playing"))
                Res.okCbor()
            }
            else -> Res.errCbor("unknown_method", "unknown method: $m")
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
