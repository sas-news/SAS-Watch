package dev.sasnews.amoledwatch

import com.github.takahirom.roborazzi.ExperimentalRoborazziApi
import com.github.takahirom.roborazzi.captureRoboImage
import com.github.takahirom.roborazzi.roborazziSystemPropertyOutputDirectory
import dev.sasnews.amoledwatch.agent.AgentConfig
import dev.sasnews.amoledwatch.agent.AgentEngine
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.media.MediaBridge
import dev.sasnews.amoledwatch.protocol.AlarmEntry
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.DeviceInfo
import dev.sasnews.amoledwatch.protocol.HelloResult
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import dev.sasnews.amoledwatch.protocol.StepsInfo
import dev.sasnews.amoledwatch.protocol.ThemePackage
import dev.sasnews.amoledwatch.ui.screens.AlarmContent
import dev.sasnews.amoledwatch.ui.screens.DevicesContent
import dev.sasnews.amoledwatch.ui.screens.DevicesUiState
import dev.sasnews.amoledwatch.ui.screens.FoundUi
import dev.sasnews.amoledwatch.ui.screens.FirmwareSendState
import dev.sasnews.amoledwatch.ui.screens.MemoContent
import dev.sasnews.amoledwatch.ui.screens.MemoUi
import dev.sasnews.amoledwatch.ui.screens.NotifyContent
import dev.sasnews.amoledwatch.protocol.OtaStatusInfo
import dev.sasnews.amoledwatch.ui.screens.ReleaseUi
import dev.sasnews.amoledwatch.ui.screens.UpdateContent
import dev.sasnews.amoledwatch.ui.screens.UpdateUiState
import dev.sasnews.amoledwatch.ui.screens.WifiContent
import dev.sasnews.amoledwatch.ui.screens.WifiUiState
import dev.sasnews.amoledwatch.ui.screens.SettingsContent
import dev.sasnews.amoledwatch.ui.screens.ThemeItemUi
import dev.sasnews.amoledwatch.ui.screens.ThemeTransferState
import dev.sasnews.amoledwatch.ui.screens.ThemesContent
import dev.sasnews.amoledwatch.ui.screens.ThemesUiState
import dev.sasnews.amoledwatch.ui.theme.AmoledWatchTheme
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

/**
 * 主要画面のスクリーンショット（Roborazzi + Robolectric）。
 * `./gradlew :app:recordRoborazziDebug` で PNG を build/outputs/roborazzi に出す。
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [34])
@OptIn(ExperimentalRoborazziApi::class)
class ScreenshotTest {

    private fun path(name: String) = "${roborazziSystemPropertyOutputDirectory()}/$name.png"

    @Test
    fun devices_connected_fake() {
        captureRoboImage(filePath = path("devices_connected")) {
            AmoledWatchTheme {
                DevicesContent(
                    state = DevicesUiState(
                        permissionsGranted = true,
                        linkState = LinkState.Connected,
                        scanning = false,
                        found = listOf(FoundUi("SAS-Watch", "AA:BB:CC:DD:EE:FF")),
                        hello = HelloResult(1, "0.1.0-fake", listOf("timer", "stopwatch", "counter", "memo", "theme")),
                        deviceInfo = DeviceInfo(87, false, "0.1.0-fake", 180_000, 7_200_000),
                        events = listOf("電池 87%（電池駆動）", "タイマー終了"),
                        log = listOf("12:00:01 → hello", "12:00:02 ← hello: ok", "12:00:02 EVT battery"),
                        isFake = true,
                        debugBuild = true,
                    ),
                    onRequestPermissions = {},
                    onScanToggle = {},
                    onConnect = {},
                    onConnectFake = {},
                    onDisconnect = {},
                    onRefresh = {},
                    onTimerStart = {},
                    onTimerStop = {},
                    onMemoSend = {},
                    onFakeBattery = {},
                    onFakeMediaCmd = {},
                    onFakeVoiceMemo = {},
                )
            }
        }
    }

    @Test
    fun devices_disconnected() {
        captureRoboImage(filePath = path("devices_disconnected")) {
            AmoledWatchTheme {
                DevicesContent(
                    state = DevicesUiState(
                        permissionsGranted = true,
                        linkState = LinkState.Disconnected,
                        scanning = false,
                        found = listOf(
                            FoundUi("SAS-Watch", "AA:BB:CC:DD:EE:FF"),
                            FoundUi(null, "11:22:33:44:55:66"),
                        ),
                        hello = null,
                        deviceInfo = null,
                        events = emptyList(),
                        log = listOf("12:00:01 BLE 接続を開始: AA:BB:CC:DD:EE:FF"),
                        isFake = false,
                        debugBuild = true,
                    ),
                    onRequestPermissions = {},
                    onScanToggle = {},
                    onConnect = {},
                    onConnectFake = {},
                    onDisconnect = {},
                    onRefresh = {},
                    onTimerStart = {},
                    onTimerStop = {},
                    onMemoSend = {},
                    onFakeBattery = {},
                    onFakeMediaCmd = {},
                    onFakeVoiceMemo = {},
                )
            }
        }
    }

    @Test
    fun memo_screen() {
        captureRoboImage(filePath = path("memo")) {
            AmoledWatchTheme {
                MemoContent(
                    connected = true,
                    memos = listOf(
                        MemoUi(id = 12, kind = "voice", sec = 3, text = null),
                        MemoUi(
                            id = 11,
                            kind = "text",
                            sec = 0,
                            text = "買い物: 牛乳・卵・パン",
                        ),
                        MemoUi(
                            id = 10,
                            kind = "text",
                            sec = 0,
                            text = "時計のアイデア: 音声メモをBLEで転送する",
                        ),
                    ),
                    playingId = null,
                    busyId = null,
                    onRefresh = {},
                    onPlay = {},
                    onDelete = {},
                    onShare = {},
                )
            }
        }
    }

    @Test
    fun settings_screen() {
        captureRoboImage(filePath = path("settings")) {
            AmoledWatchTheme {
                SettingsContent(
                    settings = mapOf(
                        SettingsKeys.BRIGHTNESS to Cbor.Cint(70),
                        SettingsKeys.DIM_AFTER_S to Cbor.Cint(8),
                        SettingsKeys.SCREEN_OFF_AFTER_S to Cbor.Cint(12),
                        SettingsKeys.BUTTON_BOOT_SHORT to Cbor.Ctext("timer.start"),
                        SettingsKeys.BUTTON_BOOT_LONG to Cbor.Ctext("nav.dev"),
                        SettingsKeys.BUTTON_BOOT_DOUBLE to Cbor.Ctext("memo.record"),
                        SettingsKeys.BUTTON_PWR_SHORT to Cbor.Ctext("back"),
                        SettingsKeys.BUTTON_PWR_LONG to Cbor.Ctext("power_menu"),
                        SettingsKeys.BUTTON_PWR_DOUBLE to Cbor.Ctext("none"),
                        SettingsKeys.THEME to Cbor.Ctext("standard"),
                        SettingsKeys.RAISE_TO_WAKE to Cbor.Cint(1),
                        SettingsKeys.STEPS_GOAL to Cbor.Cint(8000),
                        SettingsKeys.AGENT_Q1 to Cbor.Ctext("今日の予定は？"),
                        SettingsKeys.AGENT_Q2 to Cbor.Ctext("今の天気は？"),
                        SettingsKeys.AGENT_Q3 to Cbor.Ctext(""),
                    ),
                    steps = StepsInfo(2450, 8000),
                    connected = true,
                    onLoad = {},
                    onSave = {},
                    onRefreshSteps = {},
                    agentConfig = AgentConfig(),
                    agentHistory = listOf(
                        AgentEngine.Turn("今日の天気は？", "晴れです"),
                        AgentEngine.Turn("今日の予定は？", "15時に会議があります"),
                    ),
                    agentBusy = 0,
                    onSaveAgent = {},
                    onClearAgentHistory = {},
                )
            }
        }
    }

    @Test
    fun themes_screen() {
        captureRoboImage(filePath = path("themes")) {
            AmoledWatchTheme {
                ThemesContent(
                    state = ThemesUiState(
                        connected = true,
                        currentTheme = "standard",
                        builtins = listOf(
                            ThemeItemUi("standard", "標準（ダーク）", applied = true),
                            ThemeItemUi("light", "ライト"),
                        ),
                        bundled = ThemePackage.ThemePkgInfo("mame", "まめ"),
                        picked = null,
                        pickError = null,
                        transfer = ThemeTransferState.Idle,
                    ),
                    onApplyBuiltin = {},
                    onSendBundled = {},
                    onPickZip = {},
                    onSendPicked = {},
                    onRetry = {},
                )
            }
        }
    }

    @Test
    fun themes_sending() {
        captureRoboImage(filePath = path("themes_sending")) {
            AmoledWatchTheme {
                ThemesContent(
                    state = ThemesUiState(
                        connected = true,
                        currentTheme = "standard",
                        builtins = listOf(
                            ThemeItemUi("standard", "標準（ダーク）", applied = true),
                            ThemeItemUi("light", "ライト"),
                        ),
                        bundled = ThemePackage.ThemePkgInfo("mame", "まめ"),
                        picked = ThemePackage.ThemePkgInfo("yoru", "よる"),
                        pickError = null,
                        transfer = ThemeTransferState.Sending(320_000, 622_000),
                    ),
                    onApplyBuiltin = {},
                    onSendBundled = {},
                    onPickZip = {},
                    onSendPicked = {},
                    onRetry = {},
                )
            }
        }
    }

    @Test
    fun alarm_screen() {
        captureRoboImage(filePath = path("alarm")) {
            AmoledWatchTheme {
                AlarmContent(
                    connected = true,
                    alarms = listOf(
                        AlarmEntry(id = 1, hour = 7, min = 0, dow = 0x3E, on = true),
                        AlarmEntry(id = 2, hour = 9, min = 30, dow = 0, on = false),
                    ),
                    onRefresh = {},
                    onSave = { _, _, _, _, _ -> },
                    onDelete = {},
                )
            }
        }
    }

    @Test
    fun update_screen() {
        captureRoboImage(filePath = path("update")) {
            AmoledWatchTheme {
                UpdateContent(
                    state = UpdateUiState(
                        connected = true,
                        fw = "0.1.0",
                        release = ReleaseUi("v0.2.0", "https://example.com/firmware.bin"),
                        releaseLoading = false,
                        releaseError = false,
                        ota = null,
                        send = FirmwareSendState.Idle,
                        pickedName = null,
                    ),
                    onOpenWifi = {},
                    onFetchRelease = {},
                    onStartHttps = {},
                    onPickFirmware = {},
                    onSendFirmware = {},
                )
            }
        }
    }

    @Test
    fun update_progress() {
        captureRoboImage(filePath = path("update_progress")) {
            AmoledWatchTheme {
                UpdateContent(
                    state = UpdateUiState(
                        connected = true,
                        fw = "0.1.0",
                        release = ReleaseUi("v0.2.0", "https://example.com/firmware.bin"),
                        releaseLoading = false,
                        releaseError = false,
                        ota = OtaStatusInfo(
                            active = true,
                            stage = "download",
                            pct = 42,
                            msg = "",
                            version = "v0.2.0",
                        ),
                        send = FirmwareSendState.Sending(620_000, 2_500_000),
                        pickedName = "firmware.bin",
                    ),
                    onOpenWifi = {},
                    onFetchRelease = {},
                    onStartHttps = {},
                    onPickFirmware = {},
                    onSendFirmware = {},
                )
            }
        }
    }

    @Test
    fun wifi_screen() {
        captureRoboImage(filePath = path("wifi")) {
            AmoledWatchTheme {
                WifiContent(
                    state = WifiUiState(
                        connected = true,
                        savedSsid = "sas-home",
                    ),
                    onSave = { _, _ -> },
                    onBack = {},
                )
            }
        }
    }

    @Test
    fun notify_screen() {
        captureRoboImage(filePath = path("notify")) {
            AmoledWatchTheme {
                NotifyContent(
                    hasAccess = true,
                    enabledPackages = setOf("com.spotify.music"),
                    apps = listOf(
                        "com.spotify.music" to "Spotify",
                        "jp.co.sony.himedia" to "X-アプリ",
                        "com.example.mail" to "メール",
                    ),
                    nowPlaying = MediaBridge.NowPlaying("時をかける少女", "椎名林檎", true),
                    onToggle = { _, _ -> },
                    onOpenNotifSettings = {},
                )
            }
        }
    }
}
