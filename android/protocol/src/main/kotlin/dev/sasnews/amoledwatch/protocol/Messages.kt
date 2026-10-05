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

    data class MediaState(val title: String, val artist: String, val playing: Boolean) : Req {
        override val method get() = "media.state"
        override fun params() = Cbor.Cmap(
            mapOf(
                "title" to Cbor.Ctext(title), "artist" to Cbor.Ctext(artist),
                "playing" to Cbor.Cbool(playing),
            ),
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

    /** 時計→スマホへの音楽操作。 */
    data class MediaCommand(val cmd: MediaCmd) : Evt {
        override val name get() = "media.cmd"
        override val data get() = Cbor.Cmap(mapOf("cmd" to Cbor.Ctext(cmd.wire)))
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
                "media.cmd" -> MediaCommand(MediaCmd.of(d.text("cmd")) ?: return Unknown(name, d))
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

    /** kKeys と同じ順。 */
    val ALL = listOf(
        BRIGHTNESS, DIM_AFTER_S, SCREEN_OFF_AFTER_S, DEEP_SLEEP_AFTER_S,
        TZ_OFFSET_MIN, THEME,
        BUTTON_BOOT_SHORT, BUTTON_BOOT_LONG, BUTTON_BOOT_DOUBLE,
        BUTTON_PWR_SHORT, BUTTON_PWR_LONG, BUTTON_PWR_DOUBLE,
        AUDIO_VOLUME, AUDIO_CLICK,
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
    )
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
