# 実機確認手順 (batch2: T5 perf / T9 / T10 / T11 / T12)

時計は `/tmp/batch2_boot.log` 時点のファーム (batch2 最終ビルド 0x2ece00) が
書き込み済み。以下の確認をお願いします。ログ取得は全部
`timeout <秒> python3 -c 'import serial,time,sys; s=serial.Serial("/dev/ttyACM0",115200,timeout=0.5); s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False); e=time.time()+<秒>; [sys.stdout.buffer.write(d) for d in iter(lambda: s.read(8192) or time.time()>e or b"", b"")]'` 系ではなく、
単純に `idf.py -p /dev/ttyACM0 monitor` か、下の各節のワンライナーで OK。

---

## 1. T5 perf 比較 (before/after バイナリ)

**目的**: T5 (grad 廃止・anim_ms=0・glow 上限) で画面描画が速くなったかを
`perf: screen=<name> render=<ms>` 行で比較する。

**手順 (before)**:
```bash
cd /home/sasnews/Desktop/ai-space/SAS-Watch/firmware
source ~/esp/esp-idf/export.sh
python -m esptool --chip esp32s3 -b 460800 -p /dev/ttyACM0 \
    write_flash 0x20000 /tmp/bin_before_t5/sas_watch.bin
```
起動したら、各画面を順番に 1 回ずつ開く:
home → applist → 設定 → タイマー → ストップウォッチ → メモ → 音楽 → 戻る
(画面を開くたびにログに `perf: screen=... render=...` が出る)。
60 秒のシリアルキャプチャを `/tmp/perf_before.log` に保存:
```bash
timeout 65 python3 -c "
import serial, time, sys
s = serial.Serial('/dev/ttyACM0', 115200, timeout=0.5)
s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setRTS(False)
end = time.time() + 60
while time.time() < end:
    d = s.read(8192)
    if d: sys.stdout.buffer.write(d)
" > /tmp/perf_before.log
```
**手順 (after)**: 同じことを `/tmp/bin_after_t5/sas_watch.bin` で繰り返し、
`/tmp/perf_after.log` に保存。

**集計** (`/tmp/perf_sum.py`、本 doc 末尾に同梱):
```bash
python3 /tmp/perf_sum.py /tmp/perf_before.log /tmp/perf_after.log
```
画面ごとの render ms を before/after 並べて表示する。

## 2. T9 画面フラッシュ

アラームを 1 分後に設定して鳴らす (or タイマーを 10 秒で鳴らす)。

**見るもの**: アラーム/タイマー画面が白↔黒で 500ms ごと点滅 + 2 秒ごとに振動。
60 秒で点滅だけ止まる (画面は残る)。何かタップすれば即点滅停止。

**ログ**: 特に専用行は無し (画面が見えるので実見で確認)。

## 3. T10 マイク (録音ボタン操作が必要)

メモ画面を開く → 「録音開始」ボタンが**出ている**こと
(出なければ「この個体では録音できません」が表示 = ES7210 未応答)。

**操作**: 「録音開始」→ 5 秒話す → 停止 → メモが「voice」として保存される。
電話アプリの MEMO タブで取得→再生できること (ADP1→PCM 変換は電話側)。

**見るログ行** (起動時の probe + 録音中):
```
W (1432) audio: speaker disabled (WATCH_ENABLE_SPEAKER=n)   ← 想定どおり
(ES7210 非応答なら「ES7210(0x40) no response」が出る — 出ていなければ OK)
```
録音開始時に `mic i2s channel alloc failed` が出ないこと。
録音中に BLE が切れないこと (電話が接続中のまま録音)。

**SRAM**: 起動直後 `diag: psram=5339KB free, internal heap=140KB free (min 139KB)`。
録音中に `internal heap` の min が大きく下がらないこと (目安: 120KB 以上)。

## 4. T11 アルバムアート

**事前**: 電話アプリの新しい debug APK を入れておく
(`app-debug.apk` を `adb install -r`)。

**操作**: スマホで曲を再生 (アートのある曲) → 時計で「音楽」を開く。
→ 音符アイコンのところにアルバムアートが出る。曲を変えて 2 秒以内に替わる。

**ログ**: `BULK asset 転送完了 (id=...)` が電話側 Logcat に出る
(時計側は `ble connected` 後に bulk 受信。専用ログ行なし)。

**既知の確認点**: `media.state` の `art` は sha256 先頭4B LE。
アート無しの曲では音符アイコンのまま。

## 5. T12 BULK レジューム

**操作**: テーマタブから大きめのテーマ (mame ~918KB) を送信開始 →
送信中に電話の Bluetooth を 2〜3 秒だけ OFF → ON に戻す。
→ 転送が最初からではなく途中から再開して完了すること
(電話側ログに `BULK theme 転送失敗` のあと `転送完了` が続く)。

**見るログ行** (電話側): `BULK theme 転送失敗 (N/3): internal ...` →
`BULK theme 転送完了 (id=...)`。時計側は `ble connected` 後に
bulk の続きが黙々と来る (ログ行なし、完了で `theme_store: theme install <id>`)。

---

## perf_sum.py (T5 集計用)

```python
#!/usr/bin/env python3
import re, sys
def scan(path):
    r = {}
    for m in re.finditer(r'perf: screen=(\S+) render=(\d+)ms', open(path).read()):
        r[m.group(1)] = int(m.group(2))
    return r
a, b = scan(sys.argv[1]), scan(sys.argv[2])
names = sorted(set(a) | set(b))
print(f'{"screen":20s} {"before":>8s} {"after":>8s} {"delta":>8s}')
for n in names:
    x, y = a.get(n), b.get(n)
    d = (y - x) if x is not None and y is not None else None
    print(f'{n:20s} {str(x)+"ms":>8s} {str(y)+"ms":>8s} {str(d)+"ms":>8s}')
```
(`/tmp/perf_sum.py` として保存済み)
