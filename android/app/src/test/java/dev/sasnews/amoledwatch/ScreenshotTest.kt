package dev.sasnews.amoledwatch

import com.github.takahirom.roborazzi.ExperimentalRoborazziApi
import com.github.takahirom.roborazzi.captureRoboImage
import com.github.takahirom.roborazzi.roborazziSystemPropertyOutputDirectory
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.media.MediaBridge
import dev.sasnews.amoledwatch.protocol.Cbor
import dev.sasnews.amoledwatch.protocol.DeviceInfo
import dev.sasnews.amoledwatch.protocol.HelloResult
import dev.sasnews.amoledwatch.protocol.SettingsKeys
import dev.sasnews.amoledwatch.ui.screens.DevicesContent
import dev.sasnews.amoledwatch.ui.screens.DevicesUiState
import dev.sasnews.amoledwatch.ui.screens.FoundUi
import dev.sasnews.amoledwatch.ui.screens.NotifyContent
import dev.sasnews.amoledwatch.ui.screens.SettingsContent
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
                    ),
                    connected = true,
                    onLoad = {},
                    onSave = {},
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
