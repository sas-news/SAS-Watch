# vendor/ — ローカルパッチを当てた外部コンポーネント

`managed_components/` は gitignore されていて、clean clone や CI では
レジストリの素のコードが落ちてくる。性能・安定性に効くパッチを当てた
コンポーネントはここに置き、`main/idf_component.yml` の `override_path`
で差し替えている。**managed_components/ を直接編集しないこと**
(消えるし CI に乗らない)。パッチを足したら下の表も更新する。

| ディレクトリ | 元 (レジストリ) | パッチ | 理由 |
|---|---|---|---|
| `esp32_s3_touch_amoled_2_06` | waveshare/esp32_s3_touch_amoled_2_06 2.0.0 | `bsp_display_new`: `buscfg.intr_flags = ESP_INTR_FLAG_LEVEL3` | SPI 完了割り込みが既定レベルだとタッチ押下中に ~13.5ms 遅延し、全 flush が量子化していた |
| 同上 | 同上 | `bsp_display_new`: `buscfg.isr_cpu_id = ESP_INTR_CPU_AFFINITY_1` | spi_bus_lock のマルチコア競合 (esp-idf #18177 / IDFGH-17166)。ISR と LVGL タスクが別コアだと `spi_device_acquire_bus` が永久待ちになり UI が凍る。LVGL タスクも core1 固定 (`components/ui/src/esp_display.cpp`) |
| `esp_lvgl_port` | espressif/esp_lvgl_port 2.9.0 | `src/lvgl9/esp_lvgl_port_touch.c`: 専用タッチ読取タスク + シャドウキャッシュ | FT5x06 の I2C 読取が押下中 ~14ms ブロックし、LVGL タスクを止めていた |
| `esp_lcd_sh8601` | waveshare/esp_lcd_sh8601 2.0.0 | `include/esp_lcd_sh8601.h`: QSPI pclk 40→80MHz、`CMakeLists.txt`: PRIV_REQUIRES に esp_timer | QSPI 転送時間を半分にする |

上流を更新するときは、新版を取ってきて上のパッチを当て直し、実機で
`b` ベンチ (docs/perf.md) と描画・タッチを確認する。
