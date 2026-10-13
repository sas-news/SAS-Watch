package dev.sasnews.amoledwatch.protocol

/**
 * 設定の汎用エディタ (T13) 用の純粋ロジック。
 * キーと値の型から編集 UI の種別と選択肢を決める。
 * 既知のキーには日本語ラベルと選択肢、未知のキーは生の値編集。
 */
object SettingsEditor {

    /** 編集 UI の種別。 */
    enum class Kind { Bool, Int, Text, Choice }

    /** キーの日本語ラベル (既知のみ。未知は null → キー名表示)。 */
    fun labelOf(key: String): String? = LABELS[key]

    /** 値から編集種別を決める。 */
    fun kindOf(key: String, v: Cbor): Kind = when {
        choices(key) != null -> Kind.Choice
        v is Cbor.Cbool || key in BOOL_KEYS -> Kind.Bool
        v is Cbor.Cint -> Kind.Int
        else -> Kind.Text
    }

    /** Choice 種別の選択肢 (既知キーのみ)。 */
    fun choices(key: String): List<ActionSpec>? = when {
        key == SettingsKeys.FACE -> FaceNames.ALL
        key == SettingsKeys.CLOCK_FONT -> ClockFontNames.ALL
        key.startsWith("button.") -> ActionNames.ALL
        key == SettingsKeys.USB_KEEP_AWAKE -> UsbKeepAwakeModes.ALL
        else -> null
    }

    /** Cint だけど事実上 0/1 のキー。 */
    private val BOOL_KEYS = setOf(
        SettingsKeys.AUDIO_CLICK, SettingsKeys.NOTIFY_VIBRATE,
        SettingsKeys.RAISE_TO_WAKE,
    )

    private val LABELS = mapOf(
        SettingsKeys.BRIGHTNESS to "画面の明るさ (0-100)",
        SettingsKeys.DIM_AFTER_S to "減光までの秒",
        SettingsKeys.SCREEN_OFF_AFTER_S to "画面OFFまでの秒",
        SettingsKeys.DEEP_SLEEP_AFTER_S to "スリープまでの秒",
        SettingsKeys.TZ_OFFSET_MIN to "タイムゾーン (分)",
        SettingsKeys.THEME to "テーマ",
        SettingsKeys.BUTTON_BOOT_SHORT to "BOOT 短押し",
        SettingsKeys.BUTTON_BOOT_LONG to "BOOT 長押し",
        SettingsKeys.BUTTON_BOOT_DOUBLE to "BOOT ダブル",
        SettingsKeys.BUTTON_PWR_SHORT to "PWR 短押し",
        SettingsKeys.BUTTON_PWR_LONG to "PWR 長押し",
        SettingsKeys.BUTTON_PWR_DOUBLE to "PWR ダブル",
        SettingsKeys.AUDIO_VOLUME to "音量 (0-100)",
        SettingsKeys.AUDIO_CLICK to "クリック音",
        SettingsKeys.NOTIFY_VIBRATE to "通知バイブ",
        SettingsKeys.RAISE_TO_WAKE to "腕上げ点灯",
        SettingsKeys.STEPS_GOAL to "歩数目標",
        SettingsKeys.FACE to "文字盤",
        SettingsKeys.CLOCK_FONT to "時計フォント",
        SettingsKeys.AGENT_Q1 to "AI 質問 1",
        SettingsKeys.AGENT_Q2 to "AI 質問 2",
        SettingsKeys.AGENT_Q3 to "AI 質問 3",
        SettingsKeys.USB_KEEP_AWAKE to "USB 接続時は寝ない",
    )
}
