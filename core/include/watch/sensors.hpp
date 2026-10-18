// sensors.hpp — IMU (QMI8658) の判定ロジック。
// ハード非依存なので PC でテストできる (firmware は ImuSample 経由で
// このロジックを steps Feature から呼ぶ)。閾値はすべて推測値。
#pragma once

#include <cstdint>

namespace watch {
namespace sensors {

// 腕上げ判定。加速度ベクトルの「向き」が安静時の基準から一定角度以上
// 変わった状態が続いたら 1 度だけ true。戻ったら再武装する。
// 角度だけだと「腕を前に伸ばす/腕組み」等が 60-95° 変化して区別できない
// ため、姿勢の絶対方向 (画面が顔側を向いているか) も見る。
// TODO(hw): 装着位置・左右手で face 条件は要検証 (以下は左手首・
// 実機12姿勢の計測値: 見る z<=-0.71/x>=-0.13, 非対象 x<=-0.41)。
class RaiseDetector {
 public:
  struct Config {
    float raise_deg = 30.0f;       // この角度以上の向き変化を候補に
    float return_deg = 20.0f;      // この角度未満に戻ったら再武装
    float still_deg = 15.0f;       // この角度未満なら「安静」→ 基準を追従
    int32_t hold_ms = 200;         // 向き変化を保持する必要がある時間
    int32_t cooldown_ms = 2000;    // 検出後の再検出までの最小間隔
    float base_lerp = 0.03f;       // 安静時の基準ベクトル追従率 (0-1)
    float min_g = 0.6f;            // 有効サンプルの |a| 下限 (g 単位)
    float max_g = 1.7f;            // 上限 (落下・強い揺れは向きとして扱わない)
    // 「画面が顔側を向いている」絶対姿勢条件 (IMU フレームの単位ベクトル)。
    // 実機計測: 見る姿勢は z<=-0.71 (画面上向き) かつ x>=-0.13。
    // 非対象動作は x<=-0.41 (前伸ばし/腕組み/打鍵等)。false で角度のみ。
    bool face_check = true;
    float face_z_max = -0.65f;     // 画面法線 z がこの値以下 = 顔側を向く
    float face_x_min = -0.35f;     // x がこの値以上 = 見る向きの手首回転
  };

  void configure(const Config& c) { cfg_ = c; }
  void reset();

  // 1 サンプル (mg) を入力。検出したら true (クールダウン後に再検出可)。
  bool feed(int16_t x_mg, int16_t y_mg, int16_t z_mg, int64_t now_ms);

 private:
  Config cfg_;
  // 基準となる重力方向 (単位ベクトル)。
  float rx_ = 0, ry_ = 0, rz_ = 0;
  bool ref_valid_ = false;
  // raise_deg を超え始めた時刻。0 = 超えていない。
  int64_t hold_since_ms_ = 0;
  // 検出後の再武装待ち。
  bool raised_ = false;
  int64_t fired_ms_ = 0;
};

// 歩数検出。加速度の大きさ |a| の動的成分 (ゆっくり追従する基準との差)
// がピークを超えたら 1 歩。ヒステリシス + 不応期で誤検出を抑える。
// TODO(hw): 各閾値は実機で確認 (歩行波形・ポール間隔)。
class StepDetector {
 public:
  struct Config {
    float base_lerp = 0.02f;       // 基準 (~1g) の追従率 (0-1)
    float peak_mg = 220.0f;        // これを超えたら歩行ピークとみなす
    float valley_mg = 60.0f;       // これを下回るまで次のピークを数えない
    int32_t refractory_ms = 350;   // 不応期 (歩行上限 ~3 歩/秒)
  };

  void configure(const Config& c) { cfg_ = c; }
  void reset();

  // 1 サンプル (mg)。歩数が増えたら true。
  bool feed(int16_t x_mg, int16_t y_mg, int16_t z_mg, int64_t now_ms);

 private:
  Config cfg_;
  float base_ = 0;                 // 重力ベースライン (mg)
  bool init_ = false;
  bool armed_ = true;              // valley を下回って次のピーク待ち
  int64_t last_step_ms_ = 0;
};

}  // namespace sensors
}  // namespace watch
