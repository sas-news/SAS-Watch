# Muse 連携の引き継ぎ (2026-11-18)

次の担当 (Gemini 4 Carbon 等) が最初に読む要約。詳細は `docs/handoff.md` と
`docs/muse-*.md`、規約は `AGENTS.md`。

## 0. 今の状態 (commit)

- リポジトリ: `/home/sasnews/deepseek-harness/default-workspace/SAS-Watch`
- ブランチ: `main` / HEAD = `94d64f9` (origin/main と一致・作業ツリー clean)
- 直近の流れ: 公式 Muse Gadget SDK の s3-206 ビルド → SAS-Watch ファームへの
  Phase 1 + Phase 2 ①〜⑤ + 音声 UI まで実装 → ⑥ 回帰 (core/sim/firmware) 完了。
- **有料 API は使っていない**。0 円構成のみ。

## 1. できていること

### A. 公式ファームをそのまま試す (M0)
- Meta 公式 `facebookincubator/muse-gadget-sdk` (Apache-2.0, pinned `ec976f0`) が
  SAS-Watch と同一基板を `s3-206` として公式サポート。
- `tools/muse-gadget/build.sh` で ESP-IDF v6.0.3 ビルド (CJK フォント有効、
  realtime voice off)。`muse-gadget.bin` 3,411,968B を `tools/muse-gadget/out/` に生成済み。
- `tools/muse-gadget/flash.sh`: 32MB 全量バックアップ → 書き込み → 復元案内。
- 手順: `docs/muse-gadget.md`。

### B. SAS-Watch ファームへの統合 (B-min、既定 off)
- `firmware/components/muse_link/`: SDK の chat 5 + noise_core 7 ソースを無改変で
  取り込み、SAS-Watch アダプタを実装。**IDF 5.5.1 のまま** (IDF6 不要)。
- 実装済み: 資格情報 (NVS `muse` + シリアル `M` 行モード) / Wi-Fi on-demand /
  16kHz PCM→WAV/base64 / セッション起動・送受信・RAM ガード / Muse アプリ
  (UI 字幕・ページング) / 音声 UI (15 秒音声ノート、ファイル非保存)。
- Kconfig: `CONFIG_WATCH_ENABLE_MUSE_LINK` **既定 n**。off は 0x216550
  (2,188,624B) で muse_link のコード 0B。on は 0x231800。
- 検証: core 296/296、muse_link ホストテスト 4本 all pass、sim 監査 0 件、
  off/on ビルド成功。docs スクショは `docs/screenshots/54〜58_muse_*`。
- 手順・設計・実測: `docs/muse-bmin.md`、移植性評価: `docs/muse-port.md`、
  コンパイル・スパイク: `docs/muse-chat-spike.md`。

### C. M1 (資格情報) の準備
- `tools/muse-gadget/export_credentials.py`: 公式ファームの NVS
  (`0x11000`/`0xC000`) から namespace `muse` の host/vm/token を抽出
  (値を画面に出さない。`--check` / `--out` / `--shell`)。
- `tools/muse-gadget/m0_m1.sh`: M0→M1 の半自動ランブック (既定 dry-run)。
- 手順: `docs/muse-m1.md`。Muse の NVS は namespace `muse` / キー host・vm・token。

## 2. まだできていないこと (実機待ち / 要ユーザー操作)

1. **実機検証**: 時計が USB 未接続 (`/dev/ttyACM*` なし)。書き込み・M0 ペアリング・
   実録音・`muse send`→`muse reply` の往復は未実施。
2. **M1 GO/NO-GO**: 公式ファームでペアリング後、NVS から資格情報を抽出して
   SAS-Watch へ投入し、`muse start` / `muse send` / `muse reply` で判定。
3. **実機 RAM/電池**: TLS 内部 RAM (内蔵 18〜21KB に対し 30〜50KB 必要か)、
   PSRAM 1.3MB + 音声 480KB (テーマ併用)、Wi-Fi 常時接続の電池。
4. **BLE ペアリングの移植は未実施** (T47 評価で 9〜15 人日、IDF6 専用 API のため
   当面見送り)。B-min は「公式ファームでペアリング → 資格情報を移す」方式。
5. **音声の実送信**・`@chat` フックの実機挙動・muse_link の停止 API 不在
   (停止後 PSRAM ~1.3MB が残る) は既知の課題 (`docs/muse-bmin.md`)。

## 3. 環境 (このマシン)

| もの | 場所 |
|---|---|
| SAS-Watch リポジトリ | `/home/sasnews/deepseek-harness/default-workspace/SAS-Watch` |
| ESP-IDF 5.5.1 (本体ファーム) | `.toolchain/esp-idf` + `IDF_TOOLS_PATH=.toolchain/espressif` + `.toolchain/comp-cache` |
| ESP-IDF 6.0.3 (公式 Muse ファーム) | `.toolchain/esp-idf-v6.0.3` + `.toolchain/espressif-v6` |
| Muse Gadget SDK ミラー | `.toolchain/muse-gadget-sdk` (pinned `ec976f0`、再 clone しない) |
| muse2api (スマホ/muse2api 経路用、任意) | `/home/sasnews/deepseek-harness/default-workspace/services/muse2api` (Docker, port 18610, Tailscale Serve 8445) |
| 生成物 | `tools/muse-gadget/out/muse-gadget.bin`, `firmware/build/sas_watch.bin`, `android/app/build/outputs/apk/debug/app-debug.apk` |

ビルド前に必ず:
```sh
cd SAS-Watch
tools/fix_deps_lock.sh          # 作者マシンの絶対パスを読み替え
(cd firmware && . ../../.toolchain/esp-idf/export.sh && IDF_CCACHE_ENABLE=1 idf.py build)
git checkout -- firmware/dependencies.lock   # ローカル専用の変更を戻す
```
Kconfig on でビルドする場合は `CONFIG_WATCH_ENABLE_MUSE_LINK=y`。

## 4. 秘密・資格情報

- リポジトリ/ログ/チャットに実トークンは入れていない。
- 必要なもの (ユーザーが用意):
  - Muse Gadget SDK token (`mgst_...`、gadgets.muse.ai)
  - M0 ペアリング後に NVS から抽出する host/vm/token
- 値は会話・コミット・ログに出さない。`export_credentials.py` の `--out/--shell`
  は 0600 で書き、使い終わったら削除する。

## 5. 次の一手 (推奨順)

1. 時計を USB 接続 → `tools/muse-gadget/m0_m1.sh --backup --flash-muse --execute`
2. Muse アプリ (Developer mode) でペアリング + Wi-Fi → SDK token をシリアルで投入
3. `m0_m1.sh --dump-nvs --check --export --execute` → `--restore-saswatch --execute`
4. SAS-Watch を on ビルドして書き込み → `--feed-serial --execute` → `muse status`
5. `muse start` → `muse send "<短文>"` → `muse reply` の長さで M1 GO/NO-GO
6. GO なら実機で TLS/RAM/PSRAM/電池を測り、必要なら mbedTLS の PSRAM 化・
   停止 API・音声バッファ再利用を実装。NG なら A (公式ファーム) 運用 or
   現行スマホ+muse2api を継続。
