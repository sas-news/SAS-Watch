package dev.sasnews.amoledwatch.protocol

/**
 * REQ / RES / EVT の型付きモデル（protocol-v1.md §REQ/RES/EVT）。
 * REQ/RES/EVT の payload は CBOR map。
 */

// ---------- REQ ----------

sealed interface Req {
    val method: String
    fun params(): Cbor.Cmap

    data class Hello(val proto: Int = PROTO_VERSION, val app: String, val os: String = "android") : Req {
        override val method get() = "hello"
        override fun params() = Cbor.Cmap(mapOf("proto" to Cbor.Cint(proto.toLong()), "app" to Cbor.Ctext(app), "os" to Cbor.Ctext(os)))
    }

    /** epoch = 秒、tzOffsetMin = UTC からのオフセット（分）。 */
    data class TimeSet(val epoch: Long, val tzOffsetMin: Int) : Req {
        override val method get() = "time.set"
        override fun params() = Cbor.Cmap(mapOf("epoch" to Cbor.Cint(epoch), "tz_offset_min" to Cbor.Cint(tzOffsetMin.toLong())))
    }

    data object DeviceInfo : Req {
        override val method get() = "device.info"
        override fun params() = Cbor.Cmap(emptyMap())
    }

    /** keys が空なら全設定を返す。 */
    data class SettingsGet(val keys: List<String> = emptyList()) : Req {
        override val method get() = "settings.get"
        override fun params() = Cbor.Cmap(mapOf("keys" to Cbor.Carray(keys.map { Cbor.Ctext(it) })))
    }

    data class SettingsSet(val values: Map<String, Cbor>) : Req {
        override val method get() = "settings.set"
        override fun params() = Cbor.Cmap(values)
    }

    data class TimerStart(val seconds: Int) : Req {
        override val method get() = "timer.start"
        override fun params() = Cbor.Cmap(mapOf("seconds" to Cbor.Cint(seconds.toLong())))
    }

    data object TimerStop : Req {
        override val method get() = "timer.stop"
        override fun params() = Cbor.Cmap(emptyMap())
    }

    data class MemoCreate(val text: String) : Req {
        override val method get() = "memo.create"
        override fun params() = Cbor.Cmap(mapOf("text" to Cbor.Ctext(text)))
    }

    /** メモ一覧 (i=開始index, n=最大件数 1..16)。新しい順に返る。 */
    data class MemoList(val i: Int = 0, val n: Int = 16) : Req {
        override val method get() = "memo.list"
        override fun params() = Cbor.Cmap(mapOf("i" to Cbor.Cint(i.toLong()), "n" to Cbor.Cint(n.toLong())))
    }

    data class MemoGet(val id: Int) : Req {
        override val method get() = "memo.get"
        override fun params() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong())))
    }

    data class MemoDelete(val id: Int) : Req {
        override val method get() = "memo.delete"
        override fun params() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong())))
    }

    /** 音声メモの実体取得。RES 直後に時計から BULK kind="memo" が送られる。 */
    data class MemoAudioGet(val id: Int) : Req {
        override val method get() = "memo.audio.get"
        override fun params() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong())))
    }

    data class NotifyPost(val app: String, val title: String, val body: String) : Req {
        override val method get() = "notify.post"
        override fun params() = Cbor.Cmap(
            mapOf("app" to Cbor.Ctext(app), "title" to Cbor.Ctext(title), "body" to Cbor.Ctext(body)),
        )
    }

    /** 時計の Wi-Fi 資格情報を NVS に保存。pass は 0文字(オープン) or 8-63文字。読み出せない。 */
    data class WifiSet(val ssid: String, val pass: String) : Req {
        override val method get() = "wifi.set"
        override fun params() = Cbor.Cmap(
            mapOf("ssid" to Cbor.Ctext(ssid), "pass" to Cbor.Ctext(pass)),
        )
    }

    /** 保存済み Wi-Fi の有無と SSID を返す (pass は含まれない)。 */
    data object WifiStatus : Req {
        override val method get() = "wifi.status"
        override fun params() = Cbor.Cmap(emptyMap())
    }

    /** HTTPS OTA 開始。url は http(s)://、sha256 は32 bytes、version は表示用。 */
    data class OtaStart(val url: String, val sha256: ByteArray, val version: String) : Req {
        override val method get() = "ota.start"
        override fun params() = Cbor.Cmap(
            mapOf(
                "url" to Cbor.Ctext(url),
                "sha256" to Cbor.Cbytes(sha256),
                "version" to Cbor.Ctext(version),
            ),
        )
    }

    /** OTA セッションの状態を取得する。 */
    data object OtaStatus : Req {
        override val method get() = "ota.status"
        override fun params() = Cbor.Cmap(emptyMap())
    }

    data class MediaState(val title: String, val artist: String, val playing: Boolean) : Req {
        override val method get() = "media.state"
        override fun params() = Cbor.Cmap(
            mapOf(
                "title" to Cbor.Ctext(title), "artist" to Cbor.Ctext(artist),
                "playing" to Cbor.Cbool(playing),
            ),
        )
    }

    /** アラーム一覧。{alarms:[{id,hour,min,dow,on}]} が返る。 */
    data object AlarmList : Req {
        override val method get() = "alarm.list"
        override fun params() = Cbor.Cmap(emptyMap())
    }

    /**
     * アラームの追加/更新。id=0 で新規 (RES の id が採番)。
     * hour:0-23, min:0-59, dow:曜日bit (bit0=日..bit6=土, 0=毎日), on:有効。
     */
    data class AlarmSet(val id: Int, val hour: Int, val min: Int, val dow: Int, val on: Boolean) : Req {
        override val method get() = "alarm.set"
        override fun params() = Cbor.Cmap(
            mapOf(
                "id" to Cbor.Cint(id.toLong()),
                "hour" to Cbor.Cint(hour.toLong()),
                "min" to Cbor.Cint(min.toLong()),
                "dow" to Cbor.Cint(dow.toLong()),
                "on" to Cbor.Cbool(on),
            ),
        )
    }

    data class AlarmDelete(val id: Int) : Req {
        override val method get() = "alarm.delete"
        override fun params() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong())))
    }

    /** 今日の歩数と目標を返す。 */
    data object StepsGet : Req {
        override val method get() = "steps.get"
        override fun params() = Cbor.Cmap(emptyMap())
    }

    /**
     * AI の返答を時計へ送る (agent.request / BULK kind="agent_audio" に対する応答)。
     * id は要求と同じ値。text は最大960バイト (UTF-8)。
     */
    data class AgentReply(val id: Int, val text: String) : Req {
        override val method get() = "agent.reply"
        override fun params() = Cbor.Cmap(
            mapOf("id" to Cbor.Cint(id.toLong()), "text" to Cbor.Ctext(text)),
        )
    }


    /** 型を足していない method をそのまま送るとき用。 */
    data class Raw(val m: String, val p: Cbor.Cmap = Cbor.Cmap(emptyMap())) : Req {
        override val method get() = m
        override fun params() = p
    }

    companion object {
        const val PROTO_VERSION = 1
    }
}

fun Req.toCbor(): Cbor.Cmap = Cbor.Cmap(mapOf("m" to Cbor.Ctext(method), "p" to params()))

// ---------- RES ----------

sealed interface Res {
    /** `{ "ok": true, "r": <result> }` */
    data class Ok(val result: Cbor) : Res

    /** `{ "ok": false, "e": <error code string>, "msg": <string> }` */
    data class Err(val code: String, val message: String) : Res

    companion object {
        fun fromCbor(v: Cbor): Res {
            val m = v as? Cbor.Cmap ?: return Err("bad_request", "RES is not a map")
            return if (m.bool("ok")) {
                Ok(m.value["r"] ?: Cbor.Cnull)
            } else {
                Err(m.text("e", "internal"), m.text("msg"))
            }
        }

        fun okCbor(result: Cbor = Cbor.Cmap(emptyMap())): Cbor.Cmap =
            Cbor.Cmap(mapOf("ok" to Cbor.Cbool(true), "r" to result))

        fun errCbor(code: String, message: String): Cbor.Cmap =
            Cbor.Cmap(mapOf("ok" to Cbor.Cbool(false), "e" to Cbor.Ctext(code), "msg" to Cbor.Ctext(message)))
    }
}

// ---------- EVT ----------

enum class MediaCmd(val wire: String) {
    PLAY_PAUSE("play_pause"), NEXT("next"), PREV("prev"), VOL_UP("vol_up"), VOL_DOWN("vol_down");

    companion object {
        fun of(wire: String): MediaCmd? = entries.find { it.wire == wire }
    }
}

sealed interface Evt {
    val name: String
    val data: Cbor.Cmap

    data class Battery(val level: Int, val charging: Boolean) : Evt {
        override val name get() = "battery"
        override val data get() = Cbor.Cmap(mapOf("level" to Cbor.Cint(level.toLong()), "charging" to Cbor.Cbool(charging)))
    }

    data object TimerFinished : Evt {
        override val name get() = "timer.finished"
        override val data get() = Cbor.Cmap(emptyMap())
    }

    /** kind は "text" / "voice"。sec は voice の秒数 (text では 0)。 */
    data class MemoSaved(val id: Int, val kind: String = "text", val sec: Int = 0) : Evt {
        override val name get() = "memo.saved"
        override val data get() = Cbor.Cmap(
            mapOf(
                "id" to Cbor.Cint(id.toLong()),
                "kind" to Cbor.Ctext(kind),
                "sec" to Cbor.Cint(sec.toLong()),
            ),
        )
    }

    data class MemoDeleted(val id: Int) : Evt {
        override val name get() = "memo.deleted"
        override val data get() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong())))
    }

    /** アラーム鳴動開始 (id = 鳴ったアラーム)。 */
    data class AlarmRinging(val id: Int) : Evt {
        override val name get() = "alarm.ringing"
        override val data get() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong())))
    }

    /** 時計→スマホへの音楽操作。 */
    data class MediaCommand(val cmd: MediaCmd) : Evt {
        override val name get() = "media.cmd"
        override val data get() = Cbor.Cmap(mapOf("cmd" to Cbor.Ctext(cmd.wire)))
    }

    /** OTA 進捗 {pct, stage}。stage: wifi/download/verify など。 */
    data class OtaProgress(val pct: Int, val stage: String) : Evt {
        override val name get() = "ota.progress"
        override val data get() = Cbor.Cmap(
            mapOf("pct" to Cbor.Cint(pct.toLong()), "stage" to Cbor.Ctext(stage)),
        )
    }

    /** OTA 終端 {ok, msg}。ok=true の直後に時計は再起動する。 */
    data class OtaResult(val ok: Boolean, val msg: String) : Evt {
        override val name get() = "ota.result"
        override val data get() = Cbor.Cmap(
            mapOf("ok" to Cbor.Cbool(ok), "msg" to Cbor.Ctext(msg)),
        )
    }

    data class AgentRequest(val id: Int, val text: String) : Evt {
        override val name get() = "agent.request"
        override val data get() = Cbor.Cmap(mapOf("id" to Cbor.Cint(id.toLong()), "text" to Cbor.Ctext(text)))
    }

    data class Unknown(override val name: String, override val data: Cbor.Cmap) : Evt

    fun toCbor(): Cbor.Cmap = Cbor.Cmap(mapOf("e" to Cbor.Ctext(name), "d" to data))

    companion object {
        fun fromCbor(v: Cbor): Evt? {
            val m = v as? Cbor.Cmap ?: return null
            val name = m.text("e") ?: return null
            val d = m.sub("d") ?: Cbor.Cmap(emptyMap())
            return when (name) {
                "battery" -> Battery(d.int("level").toInt(), d.bool("charging"))
                "timer.finished" -> TimerFinished
                "memo.saved" -> MemoSaved(
                    d.int("id").toInt(),
                    d.text("kind", "text"),
                    d.int("sec").toInt(),
                )
                "memo.deleted" -> MemoDeleted(d.int("id").toInt())
                "alarm.ringing" -> AlarmRinging(d.int("id").toInt())
                "media.cmd" -> MediaCommand(MediaCmd.of(d.text("cmd")) ?: return Unknown(name, d))
                "ota.progress" -> OtaProgress(d.int("pct").toInt(), d.text("stage"))
                "ota.result" -> OtaResult(d.bool("ok"), d.text("msg"))
                "agent.request" -> AgentRequest(d.int("id").toInt(), d.text("text"))
                else -> Unknown(name, d)
            }
        }
    }
}

// ---------- settings keys ----------

/** protocol-v1.md の settings キー表が唯一の正（core `settings.cpp` の `kKeys` と一致）。 */
object SettingsKeys {
    const val BRIGHTNESS = "brightness"
    const val DIM_AFTER_S = "dim_after_s"
    const val SCREEN_OFF_AFTER_S = "screen_off_after_s"
    const val DEEP_SLEEP_AFTER_S = "deep_sleep_after_s"
    const val TZ_OFFSET_MIN = "tz_offset_min"
    const val THEME = "theme"
    const val BUTTON_BOOT_SHORT = "button.boot.short"
    const val BUTTON_BOOT_LONG = "button.boot.long"
    const val BUTTON_BOOT_DOUBLE = "button.boot.double"
    const val BUTTON_PWR_SHORT = "button.pwr.short"
    const val BUTTON_PWR_LONG = "button.pwr.long"
    const val BUTTON_PWR_DOUBLE = "button.pwr.double"
    const val AUDIO_VOLUME = "audio.volume"
    const val AUDIO_CLICK = "audio.click"
    const val NOTIFY_VIBRATE = "notify.vibrate"
    const val RAISE_TO_WAKE = "raise_to_wake"
    const val STEPS_GOAL = "steps.goal"
    const val FACE = "face"
    const val CLOCK_FONT = "clock_font"
    const val AGENT_Q1 = "agent.q1"
    const val AGENT_Q2 = "agent.q2"
    const val AGENT_Q3 = "agent.q3"

    /** kKeys と同じ順。 */
    val ALL = listOf(
        BRIGHTNESS, DIM_AFTER_S, SCREEN_OFF_AFTER_S, DEEP_SLEEP_AFTER_S,
        TZ_OFFSET_MIN, THEME,
        BUTTON_BOOT_SHORT, BUTTON_BOOT_LONG, BUTTON_BOOT_DOUBLE,
        BUTTON_PWR_SHORT, BUTTON_PWR_LONG, BUTTON_PWR_DOUBLE,
        AUDIO_VOLUME, AUDIO_CLICK, NOTIFY_VIBRATE,
        RAISE_TO_WAKE, STEPS_GOAL,
        FACE, CLOCK_FONT,
        AGENT_Q1, AGENT_Q2, AGENT_Q3,
    )

    /** button.* のデフォルト値（core `settings.hpp` と一致）。 */
    val DEFAULTS: Map<String, Cbor> = linkedMapOf(
        BRIGHTNESS to Cbor.Cint(50),
        DIM_AFTER_S to Cbor.Cint(8),
        SCREEN_OFF_AFTER_S to Cbor.Cint(12),
        DEEP_SLEEP_AFTER_S to Cbor.Cint(1800),
        TZ_OFFSET_MIN to Cbor.Cint(0),
        THEME to Cbor.Ctext("standard"),
        BUTTON_BOOT_SHORT to Cbor.Ctext("primary"),
        BUTTON_BOOT_LONG to Cbor.Ctext("nav.dev"),
        BUTTON_BOOT_DOUBLE to Cbor.Ctext("memo.record"),
        BUTTON_PWR_SHORT to Cbor.Ctext("back"),
        BUTTON_PWR_LONG to Cbor.Ctext("power_menu"),
        BUTTON_PWR_DOUBLE to Cbor.Ctext("none"),
        AUDIO_VOLUME to Cbor.Cint(70),
        AUDIO_CLICK to Cbor.Cint(1),
        NOTIFY_VIBRATE to Cbor.Cint(1),
        RAISE_TO_WAKE to Cbor.Cint(1),
        STEPS_GOAL to Cbor.Cint(8000),
        FACE to Cbor.Ctext("bold"),
        CLOCK_FONT to Cbor.Ctext("auto"),
        AGENT_Q1 to Cbor.Ctext("今日の予定は？"),
        AGENT_Q2 to Cbor.Ctext("今の天気は？"),
        AGENT_Q3 to Cbor.Ctext(""),
    )
}

/** `face` に設定できる文字盤 id。watch 側の盤定義と一致。 */
object FaceNames {
    val ALL: List<ActionSpec> = listOf(
        ActionSpec("bold", "ボールド"),
        ActionSpec("analog", "アナログ"),
        ActionSpec("hud", "HUD"),
        ActionSpec("minimal", "ミニマル"),
        ActionSpec("chara_side", "キャラ（横）"),
        ActionSpec("chara_bubble", "キャラ（ふきだし）"),
    )

    fun label(name: String): String = ALL.find { it.name == name }?.labelJa ?: name
}

/** `clock_font` に設定できるフォント id。`auto` は文字盤ごとの既定。 */
object ClockFontNames {
    val ALL: List<ActionSpec> = listOf(
        ActionSpec("auto", "自動（文字盤ごと）"),
        ActionSpec("oswald", "Oswald"),
        ActionSpec("bebas", "Bebas Neue"),
        ActionSpec("orbitron", "Orbitron"),
        ActionSpec("outfit", "Outfit"),
        ActionSpec("chakra", "Chakra Petch"),
    )

    fun label(name: String): String = ALL.find { it.name == name }?.labelJa ?: name
}

/**
 * settings の `button.*` に設定できる Action 名。
 * protocol-v1.md の「Action 名」表が唯一の正
 * （core `input_mapper.cpp` の `named_actions()` と一致）。
 * `labelJa` はアプリの設定画面に出す日本語ラベル。
 */
data class ActionSpec(val name: String, val labelJa: String)

object ActionNames {
    val ALL: List<ActionSpec> = listOf(
        ActionSpec("none", "なし"),
        ActionSpec("back", "戻る"),
        ActionSpec("home", "ホーム"),
        ActionSpec("primary", "主ボタン"),
        ActionSpec("screen_off", "画面OFF"),
        ActionSpec("wake", "復帰"),
        ActionSpec("power_menu", "電源メニュー"),
        ActionSpec("nav.quick", "クイック設定"),
        ActionSpec("nav.notifications", "通知"),
        ActionSpec("nav.more", "アプリ一覧"),
        ActionSpec("nav.dev", "開発者"),
        ActionSpec("nav.agent", "エージェント"),
        ActionSpec("nav.settings", "設定"),
        ActionSpec("nav.media", "メディア"),
        ActionSpec("nav.alarm", "アラーム"),
        ActionSpec("nav.steps", "歩数"),
        ActionSpec("nav.ota", "ファーム更新"),
        ActionSpec("memo.record", "メモ録音"),
        ActionSpec("timer.start", "タイマー開始"),
        ActionSpec("timer.stop", "タイマー停止"),
        ActionSpec("stopwatch.toggle", "ストップウォッチ"),
        ActionSpec("counter.add", "カウンタ +1"),
        ActionSpec("counter.sub", "カウンタ -1"),
    )

    val NAMES: List<String> = ALL.map { it.name }

    fun label(name: String): String = ALL.find { it.name == name }?.labelJa ?: name
}

// ---------- 便利関数 ----------

/** device.info の結果を Kotlin の値に展開する。 */
data class DeviceInfo(
    val battery: Int,
    val charging: Boolean,
    val fw: String,
    val freeHeap: Long,
    val freePsram: Long,
) {
    companion object {
        fun fromCbor(v: Cbor): DeviceInfo? {
            val m = v as? Cbor.Cmap ?: return null
            return DeviceInfo(
                battery = m.int("battery", -1).toInt(),
                charging = m.bool("charging"),
                fw = m.text("fw"),
                freeHeap = m.int("free_heap", -1),
                freePsram = m.int("free_psram", -1),
            )
        }
    }
}

/** hello の RES を展開する。 */
data class HelloResult(val proto: Int, val fw: String, val caps: List<String>) {
    companion object {
        fun fromCbor(v: Cbor): HelloResult? {
            val m = v as? Cbor.Cmap ?: return null
            return HelloResult(m.int("proto").toInt(), m.text("fw"), m.textList("caps"))
        }
    }
}

// ---------- メモ (memo.list / memo.get / memo.audio.get) ----------

/** memo.list の memos[] の1件。kind は "text" / "voice"。 */
data class MemoEntry(
    val id: Int,
    val kind: String,
    val sec: Int,
    val size: Long,
) {
    companion object {
        fun fromCbor(v: Cbor): MemoEntry? {
            val m = v as? Cbor.Cmap ?: return null
            return MemoEntry(
                id = m.int("id").toInt(),
                kind = m.text("kind", "text"),
                sec = m.int("sec").toInt(),
                size = m.int("size"),
            )
        }
    }
}

/** memo.list の RES を展開する。 */
data class MemoListResult(val total: Int, val memos: List<MemoEntry>) {
    companion object {
        fun fromCbor(v: Cbor): MemoListResult? {
            val m = v as? Cbor.Cmap ?: return null
            val arr = m.value["memos"] as? Cbor.Carray ?: return null
            return MemoListResult(
                total = m.int("total").toInt(),
                memos = arr.value.mapNotNull { MemoEntry.fromCbor(it) },
            )
        }
    }
}

/** memo.get の RES を展開する (text メモは text に本文)。 */
data class MemoInfo(
    val id: Int,
    val kind: String,
    val sec: Int,
    val size: Long,
    val text: String,
) {
    companion object {
        fun fromCbor(v: Cbor): MemoInfo? {
            val m = v as? Cbor.Cmap ?: return null
            return MemoInfo(
                id = m.int("id").toInt(),
                kind = m.text("kind", "text"),
                sec = m.int("sec").toInt(),
                size = m.int("size"),
                text = m.text("text"),
            )
        }
    }
}

// ---------- アラーム (alarm.list / alarm.set / alarm.delete) ----------

/** alarm.list の alarms[] の1件。dow: bit0=日..bit6=土, 0=毎日。 */
data class AlarmEntry(
    val id: Int,
    val hour: Int,
    val min: Int,
    val dow: Int,
    val on: Boolean,
) {
    companion object {
        fun fromCbor(v: Cbor): AlarmEntry? {
            val m = v as? Cbor.Cmap ?: return null
            return AlarmEntry(
                id = m.int("id").toInt(),
                hour = m.int("hour").toInt(),
                min = m.int("min").toInt(),
                dow = m.int("dow").toInt(),
                on = m.bool("on"),
            )
        }
    }
}

/** alarm.list の RES を展開する。 */
data class AlarmListResult(val alarms: List<AlarmEntry>) {
    companion object {
        fun fromCbor(v: Cbor): AlarmListResult? {
            val m = v as? Cbor.Cmap ?: return null
            val arr = m.value["alarms"] as? Cbor.Carray ?: return null
            return AlarmListResult(arr.value.mapNotNull { AlarmEntry.fromCbor(it) })
        }
    }
}

/** alarm.set の RES を展開する ({id:N})。 */
data class AlarmSetResult(val id: Int) {
    companion object {
        fun fromCbor(v: Cbor): AlarmSetResult? {
            val m = v as? Cbor.Cmap ?: return null
            return AlarmSetResult(m.int("id").toInt())
        }
    }
}

/** wifi.status の RES を展開する。pass は返らない設計なので含まれない。 */
data class WifiStatusInfo(val configured: Boolean, val ssid: String) {
    companion object {
        fun fromCbor(v: Cbor): WifiStatusInfo? {
            val m = v as? Cbor.Cmap ?: return null
            return WifiStatusInfo(m.bool("configured"), m.text("ssid"))
        }
    }
}

/** ota.status の RES を展開する。stage: idle/wifi/download/verify/done/reboot/fail。 */
data class OtaStatusInfo(
    val active: Boolean,
    val stage: String,
    val pct: Int,
    val msg: String,
    val version: String,
) {
    companion object {
        fun fromCbor(v: Cbor): OtaStatusInfo? {
            val m = v as? Cbor.Cmap ?: return null
            return OtaStatusInfo(
                active = m.bool("active"),
                stage = m.text("stage", "idle"),
                pct = m.int("pct").toInt(),
                msg = m.text("msg"),
                version = m.text("version"),
            )
        }
    }
}

/** steps.get の RES を展開する。 */
data class StepsInfo(val steps: Long, val goal: Long) {
    companion object {
        fun fromCbor(v: Cbor): StepsInfo? {
            val m = v as? Cbor.Cmap ?: return null
            return StepsInfo(steps = m.int("steps"), goal = m.int("goal"))
        }
    }
}

/** memo.audio.get の RES を展開する。sha256 は 32 bytes。 */
data class MemoAudioInfo(val id: Int, val size: Long, val sha256: ByteArray) {
    companion object {
        fun fromCbor(v: Cbor): MemoAudioInfo? {
            val m = v as? Cbor.Cmap ?: return null
            val sha = m.value["sha256"] as? Cbor.Cbytes ?: return null
            return MemoAudioInfo(
                id = m.int("id").toInt(),
                size = m.int("size"),
                sha256 = sha.value,
            )
        }
    }
}
