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

    data class MemoSaved(val id: Int) : Evt {
        override val name get() = "memo.saved"
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
                "memo.saved" -> MemoSaved(d.int("id").toInt())
                "media.cmd" -> MediaCommand(MediaCmd.of(d.text("cmd")) ?: return Unknown(name, d))
                "agent.request" -> AgentRequest(d.int("id").toInt(), d.text("text"))
                else -> Unknown(name, d)
            }
        }
    }
}

// ---------- settings keys ----------

object SettingsKeys {
    const val BRIGHTNESS = "brightness"
    const val DIM_AFTER_S = "dim_after_s"
    const val SCREEN_OFF_AFTER_S = "screen_off_after_s"
    const val BUTTON_BOOT_SHORT = "button.boot.short"
    const val BUTTON_BOOT_LONG = "button.boot.long"
    const val BUTTON_BOOT_DOUBLE = "button.boot.double"
    const val BUTTON_PWR_SHORT = "button.pwr.short"
    const val BUTTON_PWR_LONG = "button.pwr.long"
    const val THEME = "theme"

    val ALL = listOf(
        BRIGHTNESS, DIM_AFTER_S, SCREEN_OFF_AFTER_S,
        BUTTON_BOOT_SHORT, BUTTON_BOOT_LONG, BUTTON_BOOT_DOUBLE,
        BUTTON_PWR_SHORT, BUTTON_PWR_LONG, THEME,
    )

    /** settings の値に使う Action 名。protocol-v1.md は「Action 名文字列」とだけ書くので、
     *  plan.md のボタン表＋ActionType から推測した一覧。
     * // TODO(hw): 実機で確認 — 時計側の Action 名が決まったら揃える */
    val BUTTON_ACTIONS = listOf(
        "nav.back",
        "nav.home",
        "nav.quick",
        "nav.dev",
        "power.menu",
        "screen.off",
        "timer.toggle",
        "stopwatch.toggle",
        "counter.add",
        "memo.record",
        "none",
    )
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
