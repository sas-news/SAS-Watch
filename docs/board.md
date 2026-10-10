# Board: Waveshare ESP32-S3-Touch-AMOLED-2.06

出典：公式回路図 `Schematic/ESP32-S3-Touch-AMOLED-2.06-Schematic-V1.0.pdf`、公式BSP `waveshare/esp32_s3_touch_amoled_2_06`、公式 ESP-IDF サンプル。
【要実機】は実機で確認して更新すること。

## 部品
| 部品 | 型番 | バス / アドレス |
|---|---|---|
| SoC | ESP32-S3R8 (PSRAM 8MB Octal) | |
| Flash | GD25Q256 (32MB)。flash_id 実測済み・32MB 全量使用（ESP32-S3 は 32bit アドレスマップ対応で実験的オプション不要） | |
| Display | 410x502 AMOLED, CO5300 (BSP は esp_lcd_sh8601 で駆動) | QSPI |
| Touch | FT3168 (BSP は esp_lcd_touch_ft5x06) | I2C 0x38 |
| PMIC | AXP2101 (XPowersLib) | I2C 0x34 |
| IMU | QMI8658C (waveshare/qmi8658) | I2C 0x6B |
| RTC | PCF85063ATL | I2C 0x51 |
| Mic ADC | ES7210 + mic x2 | I2C 0x40 【要実機】 + I2S |
| Codec | ES8311 + NS4150B amp | I2C 0x18 【要実機】 + I2S |
| microSD | BSP は SDMMC 1-bit | |
| Motor | パッドのみ (GPIO18→NPN)。本体の有無【要実機】 | |

## GPIO
| GPIO | 用途 | RTC GPIO | 備考 |
|---|---|---|---|
| 0 | BOOT ボタン (押下 LOW) | yes | ext0 wake |
| 1/2/3/17 | SD CMD/CLK/D0/CS | | |
| 4,5,6,7 | QSPI SIO0-3 | | |
| 8 | LCD RESET | | |
| 9 | TP RESET | | |
| 10 | PWR ボタン SYS_OUT (押下 HIGH) | yes | ext1 wake。長押しは AXP2101 がハード電源断 |
| 11 | QSPI SCL | | |
| 12 | LCD CS | | |
| 13 | LCD TE | | BSP 未使用 |
| 14/15 | I2C SCL/SDA (全デバイス共通) | | |
| 16 | I2S MCLK | | |
| 18 | Motor | | |
| 19/20 | USB D-/D+ | | |
| 21 | QMI8658 INT1 | yes | ext1 wake (wake-on-motion)。極性【要実機】 |
| 38 | TP INT | no | light sleep wake のみ |
| 39 | RTC INT | no | light sleep wake のみ |
| 40 | I2S DSDIN | | |
| 41 | I2S SCLK | | |
| 42 | I2S ASDOUT | | |
| 43/44 | UART0 TX/RX | | |
| 45 | I2S LRCK | | |
| 46 | PA_CTRL (amp enable) | | |

## 電源レール (AXP2101)
DCDC1=3.3V(VCC3V3, ESP32/パネルVCI) / ALDO2=3.3V → DSI_PWR_EN (パネル電源) / ALDO3 → モーター / AXP IRQ は ESP32 に未接続（I2C ポーリング）。

## BSP の事実
- `bsp_display_start()`：LVGL バッファ 410x20 行、PSRAM、シングル
- `bsp_display_brightness_set(percent)`：CO5300 cmd 0x51
- `bsp_display_lock(timeout)` / `bsp_display_unlock()`
- `bsp_i2c_init()` / `bsp_i2c_get_handle()`
- `bsp_audio_codec_speaker_init()` / `bsp_audio_codec_microphone_init()`
- `bsp_sdcard_mount()`
- BSP_CAPS_BUTTONS=0, BSP_CAPS_IMU=0（ボタンと IMU は自前）
- CO5300 は描画領域の x/y が偶数である必要あり（rounder）【要実機】

## 未確認リスト
- [ ] Flash ID (`esptool.py flash_id`)
- [ ] I2C スキャン結果
- [ ] PWR=GPIO10 で HIGH になるか
- [ ] QMI8658 INT1 の極性
- [ ] モーターの有無
- [ ] 画面向き・タッチ座標
- [ ] 各状態の消費電流
