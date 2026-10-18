#include "board/board.hpp"
#include "pins.hpp"

#include <cstdio>
#include <cmath>

#include "esp_timer.h"
#include "esp_attr.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/rtc_io_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// docs/board.md / plan.md A-3:
// - deep sleep から復帰できるのは PWR(GPIO10), IMU(GPIO21), timer だけ
//   (GPIO38 タッチ, GPIO39 RTC は RTC GPIO ではないので deep sleep では使えない)
// - BOOT(GPIO0) は RTC GPIO だが strapping pin でもある: deep sleep 起床は
//   チップリセットなので、起床のために押した BOOT の LOW がそのまま
//   ストラップサンプルに残り、UART download mode で起動してしまう
//   (= 復帰不能に見える)。そのため ext0 (BOOT) は deep sleep では張らない。
// - light sleep なら GPIO38 タッチや BOOT でも復帰できる

namespace board::sleep {

static const char* TAG = "board.sleep";

// deep sleep を跨ぐ基準姿勢 (RTC 保持メモリ)。入眠時の重力方向を単位
// ベクトルで持ち、WoM 起床時に「本当に腕を上げたか」の判定に使う。
namespace {
RTC_DATA_ATTR float s_ref[3];
RTC_DATA_ATTR uint32_t s_ref_magic;
RTC_DATA_ATTR uint64_t s_prior_wake_us;
constexpr uint32_t kRefMagic = 0x52454631u;  // "REF1"
// 起床ゲートの判定結果 — USB ポートが開く前のログは喪失するので
// RTC 保持メモリに残して起動後の診断ダンプから拾う。
RTC_DATA_ATTR int32_t s_gate_n;       // 採用サンプル数 (-1=基準無し)
RTC_DATA_ATTR int32_t s_gate_deg_x10;
RTC_DATA_ATTR int16_t s_gate_dir_x10[3];  // 計測姿勢 (単位ベクトル x10)
RTC_DATA_ATTR uint32_t s_gate_magic;
constexpr uint32_t kGateMagic = 0x47415445u;  // "GATE"
// 入眠直前の INT1 LOW 待ち結果 — 直前ログは USB 切断で喪失するので
// RTC 保持して次ブートの診断ダンプから拾う。
RTC_DATA_ATTR int32_t s_entry_iters;  // LOW 待ちループ反復数 (20=打切り)
RTC_DATA_ATTR int32_t s_entry_lvl;    // 入眠直前の INT1 実レベル
RTC_DATA_ATTR uint32_t s_entry_magic;
constexpr uint32_t kEntryMagic = 0x454E5452u;  // "ENTR"
}

void arm_light_sleep_wake()
{
    // LOW アクティブ: タッチ INT / BOOT ボタン
    gpio_wakeup_enable(pins::kTouchInt, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(pins::kButtonBoot, GPIO_INTR_LOW_LEVEL);
    // HIGH アクティブ: PWR ボタン
    gpio_wakeup_enable(pins::kButtonPwr, GPIO_INTR_HIGH_LEVEL);
    // LOW アクティブ: RTC INT (アラーム発火)。deep sleep では使えない
    // (RTC GPIO ではない) ので light sleep 専用。
    gpio_wakeup_enable(pins::kRtcInt, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    ESP_LOGI(TAG, "light sleep wake: touch(GPIO38,LOW) boot(GPIO0,LOW) pwr(GPIO10,HIGH) rtc(GPIO39,LOW)");
}

void arm_light_sleep_imu()
{
    // GPIO21 は imu_service が GPIO 割り込み (POSEDGE) で WoM を受けるので
    // gpio_wakeup_enable (LEVEL 型に int 型を書き換える) は呼べない。
    // light sleep が現状未使用のため何もしない。
    // TODO(hw): light sleep 実装時に再検討 (割り込み型の共存 or 排他)
}

void disarm_light_sleep_imu()
{
    gpio_wakeup_disable(pins::kImuInt1);
}

void log_pin_levels(const char* tag)
{
    // S3 には esp_sleep_get_gpio_wakeup_status が無いので、
    // 起床直後 (または入眠直前) の実レベルを出して切り分ける。
    ESP_LOGI(tag, "levels: boot=%d pwr=%d imu=%d touch=%d rtc=%d",
             gpio_get_level(pins::kButtonBoot), gpio_get_level(pins::kButtonPwr),
             gpio_get_level(pins::kImuInt1), gpio_get_level(pins::kTouchInt),
             gpio_get_level(pins::kRtcInt));
}

bool woke_by_imu()
{
    // WoM の INT1 は STATUS1 読取まで保持されるレベル出力（QMI8658C DS §6.1）
    // なので起床時にピンが HIGH なら IMU 起因とみなす。
    // TODO(hw): 実機で確認（短いパルスなら取りこぼす可能性あり）
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO &&
           gpio_get_level(pins::kImuInt1) == 1;
}

void enter_deep_sleep(uint64_t wake_after_us, bool imu_wake,
                      int32_t* rtc_level_out, uint32_t* regs_out)
{
    // light sleep 用に貼り付けた歩数同期タイマー (imu_service が set) を
    // deep sleep に持ち込まない。以降は wake_after_us だけが timer になる。
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    // arm_light_sleep_wake() の gpio_wakeup_enable は RTC_GPIO_TRIG_EN を
    // 立てる。これは deep sleep にも残り、RTC GPIO (GPIO0/GPIO10) が指定
    // レベルなら入眠直後に起きてしまう (= 復帰不能に見える or ウエイクループ)。
    // GPIO38/39 は非 RTC なので効かないが、GPIO10 (PWR) は ext1 と二重になる
    // ので GPIO 側は全て外して ext1 に一本化する。
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    // 入眠時の実レベルを残す (即起床ループの切り分け用)。
    log_pin_levels(TAG);

    // RTC 側レベルを既知ピンと対照して読み、入眠時の実レベルを診断に残す。
    //   g0 (BOOT 解放=HIGH) / g10 (PWR 解放=LOW) / g21 (IMU INT1)
    // この初期化は機能上も必要: rtc_gpio_init が LP IO クロック
    // (io_mux_enable_lp_io_clock) の参照カウントを上げるので、pad10/21 を
    // deinit せず残しておくことで深眠中の RTC パッド入力が保証される。
    uint32_t lvls = 0;
    const gpio_num_t probe[] = {GPIO_NUM_0, GPIO_NUM_10, GPIO_NUM_21};
    for (const gpio_num_t g : probe) {
        rtc_gpio_init(g);
        rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    }
    lvls |= rtc_gpio_get_level(GPIO_NUM_0);        // bit0: BOOT (expect 1)
    lvls |= rtc_gpio_get_level(GPIO_NUM_10) << 1;  // bit1: PWR  (expect 0)
    lvls |= rtc_gpio_get_level(GPIO_NUM_21) << 2;  // bit2: IMU  (期待1)
    rtc_gpio_pullup_en(GPIO_NUM_21);
    esp_rom_delay_us(50);  // プルアップが効くまで少し待つ
    lvls |= rtc_gpio_get_level(GPIO_NUM_21) << 3;  // bit3: IMU+pullup
    lvls |= gpio_get_level(GPIO_NUM_21) << 4;      // bit4: dig 側の g21
    rtc_gpio_pullup_dis(GPIO_NUM_21);
    ESP_LOGI(TAG, "rtc lvls g0=%d g10=%d g21=%d g21pu=%d dig21=%d",
             (int)(lvls & 1), (int)((lvls >> 1) & 1), (int)((lvls >> 2) & 1),
             (int)((lvls >> 3) & 1), (int)((lvls >> 4) & 1));
    // 生レジスタも診断に残す — pad21 の MUX_SEL/FUN_IE、hold 残留、in_val。
    {
        const uint32_t p21 = REG_READ(RTC_IO_RTC_PAD21_REG);
        const uint32_t p10 = REG_READ(RTC_IO_TOUCH_PAD10_REG);
        const uint32_t hold = REG_READ(RTC_CNTL_PAD_HOLD_REG);
        const uint32_t in = REG_READ(RTC_GPIO_IN_REG);
        ESP_LOGI(TAG,
                 "regs p21=%08lx(mux=%ld ie=%ld slp_sel=%ld slp_ie=%ld "
                 "rue=%ld rde=%ld) p10=%08lx hold=%08lx in=%08lx",
                 (unsigned long)p21, (unsigned long)((p21 >> 19) & 1),
                 (unsigned long)((p21 >> 13) & 1),
                 (unsigned long)((p21 >> 16) & 1),
                 (unsigned long)((p21 >> 15) & 1),
                 (unsigned long)((p21 >> 27) & 1),
                 (unsigned long)((p21 >> 28) & 1), (unsigned long)p10,
                 (unsigned long)hold, (unsigned long)in);
        if (rtc_level_out) {
            *rtc_level_out = (int32_t)lvls;
        }
        if (regs_out) {
            regs_out[0] = p21;
            regs_out[1] = p10;
            regs_out[2] = hold;
            regs_out[3] = in;
        }
    }
    // BOOT(GPIO0) だけデジタルに戻す。pad10/21 は RTC 入力の
    // まま残す — rtc_gpio_deinit は参照カウント式の LP IO クロック
    // (io_mux_enable_lp_io_clock) を最後の1ピンで落とすため、全ピン deinit
    // すると ext0/ext1 prepare (rtcio_hal 直叩き・クロック有効化無し) が
    // クロック OFF の状態でパッドを設定してしまう。
    rtc_gpio_deinit(GPIO_NUM_0);

    // PWR(GPIO10) / IMU INT1(GPIO21) — ext1, どれかが HIGH で復帰。
    // INT1 極性は実機確認済み: WoM イベントで HIGH ラッチ → ANY_HIGH 起床。
    // imu_wake=false (raise_to_wake OFF) の時に INT1 を入れると、未クリアの
    // WoM ラッチや浮いたピンの HIGH で入眠直後に即起床するので外す。
    uint64_t ext1_mask = (1ULL << pins::kButtonPwr);
    if (imu_wake) {
        ext1_mask |= (1ULL << pins::kImuInt1);
    }
    esp_err_t ret =
        esp_sleep_enable_ext1_wakeup(ext1_mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ext1 wake failed: %s", esp_err_to_name(ret));
    }
    if (imu_wake) {
        // ext0 も併用 — 別機構のレベルコンパレータで冗長化 (実機で ext0 起床
        // を確認済み)。両方立てても起床原因はどちらかが報告されるだけで害は無い。
        const esp_err_t r0 = esp_sleep_enable_ext0_wakeup(pins::kImuInt1, 1);
        ESP_LOGI(TAG, "ext0 imu wake: %s", esp_err_to_name(r0));
    }

    if (wake_after_us > 0) {
        esp_sleep_enable_timer_wakeup(wake_after_us);
    }

    ESP_LOGI(TAG, "entering deep sleep (pwr ext1, imu=%s, timer=%s)",
             imu_wake ? "on" : "off", wake_after_us > 0 ? "on" : "off");
    // 入眠直前に WoM ラッチを最終クリア (init_low では STATUS1 読取で
    // INT1 は休息 LOW に戻る)。入眠時の実レベルを残す。
    if (imu_wake) {
        board::imu::wom_clear();
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_LOGI(TAG, "final int1: dig=%d rtc=%d",
                 (int)gpio_get_level(pins::kImuInt1),
                 (int)rtc_gpio_get_level(pins::kImuInt1));
    }
    // USB-Serial/JTAG は deep sleep で切れるので、直前ログの送出を待つ
    // (100ms では TX FIFO が host に汲まれる前に切れて喪失した — 500ms)。
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));
    // フラッシュの 500ms 中に WoM イベントが発火して INT1 が HIGH
    // ラッチしていると、入眠と同時に ext0/ext1 が即発火する (実機観測:
    // 取り回しの動きで毎度即起床)。STATUS1 読取ではピンが初期値に戻ら
    // ないため、HIGH を読むたびに disarm→arm で初期値 LOW に強制する。
    // そのついでに姿勢を基準として採り直す — 手動入眠では watch_app が
    // 基準を採る時点ではまだ取り回し中で、寝姿勢とずれるとノイズ起床
    // で誤採用し得る。LOW が連続3回 (150ms) 読めたら安静確定で入眠。
    // 動き続けても上限 ~1s で寝る (その場合は起床ゲートが姿勢で判定)。
    if (imu_wake) {
        int low_streak = 0;
        int i = 0;
        for (; i < 20; ++i) {
            if (!gpio_get_level(pins::kImuInt1)) {
                if (++low_streak >= 3) break;
            } else {
                low_streak = 0;
                board::imu::disarm_wake_on_motion();
                vTaskDelay(pdMS_TO_TICKS(60));  // 21Hz で新規1発
                float ax = 0, ay = 0, az = 0;
                if (board::imu::read_accel_mg(&ax, &ay, &az) == ESP_OK) {
                    set_wake_reference(ax, ay, az, s_prior_wake_us);
                }
                board::imu::arm_wake_on_motion(false);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        s_entry_iters = i;
        s_entry_lvl = (int)gpio_get_level(pins::kImuInt1);
        s_entry_magic = kEntryMagic;
    }
    esp_deep_sleep_start();
    // ここには戻らない
    for (;;) {
    }
}

void set_wake_reference(float x, float y, float z, uint64_t prior_wake_us)
{
    const float n = sqrtf(x * x + y * y + z * z);
    if (n < 300.f) {  // 1g の 3 割未満は自由落下/未初期化 — 基準にしない
        s_ref_magic = 0;
        return;
    }
    s_ref[0] = x / n;
    s_ref[1] = y / n;
    s_ref[2] = z / n;
    s_prior_wake_us = prior_wake_us;
    s_ref_magic = kRefMagic;
    ESP_LOGI(TAG, "wake ref: n=%.0f dir=%.2f,%.2f,%.2f",
             n, s_ref[0], s_ref[1], s_ref[2]);
}

bool maybe_fast_resleep()
{
    // deep sleep からの IMU (WoM) 起床は、入眠時基準姿勢との角度差で
    // 「腕上げ」を判定する。未満なら通常起動せず即再入眠 — 装着中の
    // ただの動きで画面ON/起きっぱなしになる電池食いを防ぐ。
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool imu_cause = false;
    if (cause == ESP_SLEEP_WAKEUP_EXT0 ||
        cause == ESP_SLEEP_WAKEUP_EXT1) {
        // ext0 と ext1 は別機構 — PWR 押下と WoM 発火がほぼ同時だと
        // cause は ext0 (IMU) を報告しても ext1 status に PWR ビットが
        // 残る。ボタン押下は常にユーザーの明示操作なのでゲートを素通り。
        const uint64_t m = esp_sleep_get_ext1_wakeup_status();
        if (m & (1ULL << pins::kButtonPwr)) {
            s_gate_n = -2;  // PWR 押下 = ゲートバイパス
            s_gate_deg_x10 = 0;
            s_gate_magic = kGateMagic;
            return false;  // PWR = ユーザーの明示操作
        }
        imu_cause = (cause == ESP_SLEEP_WAKEUP_EXT0) ||
                    (m & (1ULL << pins::kImuInt1)) != 0;
    }
    if (!imu_cause) {
        return false;
    }
    if (s_ref_magic != kRefMagic) {
        s_gate_n = -1;
        s_gate_deg_x10 = 0;
        s_gate_magic = kGateMagic;
        ESP_LOGI(TAG, "imu wake gate: no ref -> normal boot");
        return false;
    }
    s_ref_magic = 0;  // 基準は使い切り — 寝直す側が改めて採る

    // QMI8658 の WoM arm は ESP リセットを跨いで持続し、arm 中は出力
    // レジスタが凍結する (実機観測 — imu_service の burst() と同じ理由)。
    // 計測前に通常ストリーミングへ戻す。
    const esp_err_t dret = imu::disarm_wake_on_motion();
    if (dret != ESP_OK) {
        ESP_LOGW(TAG, "imu wake gate: disarm failed -> normal boot");
        return false;  // 計測不能なのに寝直すと本物の腕上げを拾えない
    }
    vTaskDelay(pdMS_TO_TICKS(60));  // LP 21Hz ≈ 48ms — 新規サンプル待ち

    // 「落ち着いた姿勢」を最大 ~1.2s 待って採る。腕上げジェスチャは
    // WoM 発火の時点では動作途中で、完了まで 0.5–1s かかる — 一定時間
    // 計測して早めに判定すると姿勢変化を読み損なう。0.6–1.7g 帯のみ
    // 採用し、直近の有効サンプルと 10° 以内が 3 連続したら静止とみなす。
    // 動き続けて安定しない場合は打ち切り (誤差は拒否側=省電力側に倒す)。
    float sx = 0, sy = 0, sz = 0;   // 安定ランの単位ベクトル和
    int n = 0;
    float px = 0, py = 0, pz = 0;   // 直前の有効サンプル (単位ベクトル)
    int stable = 0;
    for (int i = 0; i < 30; ++i) {
        float ax = 0, ay = 0, az = 0;
        if (imu::read_accel_mg(&ax, &ay, &az) == ESP_OK) {
            const float nn = sqrtf(ax * ax + ay * ay + az * az);
            const float g = nn / 1000.f;
            if (g >= 0.6f && g <= 1.7f) {
                const float ux = ax / nn, uy = ay / nn, uz = az / nn;
                if (px != 0 || py != 0 || pz != 0) {
                    const float dp = ux * px + uy * py + uz * pz;
                    // cos(10°)≈0.985 — 直近とほぼ同じ向きなら静止カウント
                    if (dp >= 0.985f) {
                        ++stable;
                    } else {
                        // 向きが変わった → スイング中サンプルを捨てて
                        // 新しい安定ランを開始する (判定は静止姿勢だけで)
                        stable = 0;
                        sx = ux;
                        sy = uy;
                        sz = uz;
                        n = 1;
                    }
                } else {
                    sx = ux;
                    sy = uy;
                    sz = uz;
                    n = 1;
                }
                px = ux;
                py = uy;
                pz = uz;
                if (stable >= 1) {
                    sx += ux;
                    sy += uy;
                    sz += uz;
                    ++n;
                }
                if (stable >= 3) {
                    break;  // 姿勢が安定した → 判定へ
                }
            } else {
                stable = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    float deg = 0;
    const float cn = sqrtf(sx * sx + sy * sy + sz * sz);
    if (n > 0 && cn > 1e-3f) {
        const float d =
            (sx * s_ref[0] + sy * s_ref[1] + sz * s_ref[2]) / cn;
        deg = acosf(d < -1.f ? -1.f : (d > 1.f ? 1.f : d)) * 57.29578f;
    }
    ESP_LOGI(TAG, "imu wake gate: n=%d deg=%.1f", n, deg);
    s_gate_n = n;
    s_gate_deg_x10 = static_cast<int32_t>(deg * 10.f);
    if (cn > 1e-3f) {
        s_gate_dir_x10[0] = static_cast<int16_t>(sx / cn * 10.f);
        s_gate_dir_x10[1] = static_cast<int16_t>(sy / cn * 10.f);
        s_gate_dir_x10[2] = static_cast<int16_t>(sz / cn * 10.f);
    }
    s_gate_magic = kGateMagic;
    constexpr float kRaiseDeg = 50.f;  // core RaiseDetector::raise_deg と同値
    if (n <= 0) {
        return false;  // 有効サンプル無し = 判定不能 → 寝直さない
    }
    // 姿勢が安定しなかった (連続動作中に打ち切り) 場合は受け入れない —
    // 振動/取り回しで一瞬 50° を超えても画面ONしない。
    if (stable >= 3 && deg >= kRaiseDeg) {
        return false;  // 腕を上げて静止した → 通常起動 (画面ON)
    }
    if (pmic::is_vbus()) {
        return false;  // USB 接続中は寝直さない (デバッグ用)
    }

    // 腕上げではない → WoM を張り直して即再入眠。
    // 基準姿勢は今回計測した姿勢で更新する (使い切りにすると次の WoM 起床で
    // 判定不能になる)。timer (アラーム用 deadline) は残量を引き継ぐ。
    if (n > 0 && cn > 1e-3f) {
        s_ref[0] = sx / cn;
        s_ref[1] = sy / cn;
        s_ref[2] = sz / cn;
        s_ref_magic = kRefMagic;
    }
    imu::arm_wake_on_motion(false);
    const uint64_t elapsed_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t rem_us =
        (s_prior_wake_us > elapsed_us) ? s_prior_wake_us - elapsed_us : 0;
    ESP_LOGI(TAG, "not a raise -> resleep (timer rem=%llus)",
             (unsigned long long)(rem_us / 1000000));
    enter_deep_sleep(rem_us, true, nullptr, nullptr);
    // noreturn
    return true;
}

bool last_gate_debug(int32_t* n, float* deg, float* dir)
{
    if (s_gate_magic != kGateMagic) {
        return false;
    }
    *n = s_gate_n;
    *deg = s_gate_deg_x10 / 10.f;
    if (dir) {
        dir[0] = s_gate_dir_x10[0] / 10.f;
        dir[1] = s_gate_dir_x10[1] / 10.f;
        dir[2] = s_gate_dir_x10[2] / 10.f;
    }
    return true;
}

bool last_entry_debug(int* iters, int* lvl)
{
    if (s_entry_magic != kEntryMagic) {
        return false;
    }
    *iters = s_entry_iters;
    *lvl = s_entry_lvl;
    return true;
}

}  // namespace board::sleep
