# SAS-Watch コンパニオン（Android）

Waveshare ESP32-S3-Touch-AMOLED-2.06 自作スマートウォッチ用の Android 14 向けコンパニオンアプリ。
通信仕様は `docs/protocol-v1.md`、方針は `docs/plan.md` J 章。

## 構成

```
android/
├── protocol/   # 純 Kotlin/JVM（Android 非依存）
│               #   Frame/CRC-16/フラグメント/最小CBOR/REQ・RES・EVT モデル/FakeWatch
└── app/        # Kotlin + Jetpack Compose + Material3
                #   BLE 接続 / Foreground Service / 通知転送 / メディア連携 / 設定画面
```

- applicationId `dev.sasnews.amoledwatch`、minSdk 31、targetSdk 34、compileSdk 35
- デバッグビルドでは「デバッグ用の仮想時計」（`FakeWatch`）に接続して、実機なしで全部の画面を動かせる

## ビルド

```sh
cd android
./gradlew :protocol:test :app:assembleDebug :app:lint
```

APK は `app/build/outputs/apk/debug/app-debug.apk` にできる。

## スマホへのインストール（CI の artifact から）

1. GitHub のこのリポジトリをブラウザで開く
2. 上のタブの「Actions」を押す
3. 「android」ワークフローの一番上（最新）の行を押す
4. 下の方にある「Artifacts」の「app-debug-apk」を押して ZIP をダウンロードする
5. ZIP の中の `app-debug.apk` をスマホに送る（USB でコピー / Google Drive 等）
6. スマホで `app-debug.apk` をファイルアプリから開く
7. 「提供元不明のアプリをインストールしますか？」と出たら「許可」を押す
8. 「インストール」を押す

※ Play ストア経由ではないので「安全でない可能性があります」系の警告が出ることがある。自分でビルドしたものなので問題ない。

## 必要な権限

| 権限 | 何に使うか | いつ聞かれるか |
|---|---|---|
| Bluetooth（周辺のデバイス） | 時計をスキャンして接続する | アプリ起動時のカードから「権限を許可する」 |
| 通知（POST_NOTIFICATIONS） | 接続状態を通知欄に出す（Foreground Service） | 同上 |
| 通知アクセス | 他アプリの通知を時計に転送、再生中の曲情報の取得 | 「通知・メディア」タブの「通知アクセス設定を開く」→ 一覧で「SAS-Watch 通知転送」を ON |

Bluetooth の権限は `neverForLocation`（位置情報には使わない）で宣言している。

## 実機が届くまで（FakeWatch）

1. アプリを起動して「デバイス」タブを開く
2. 「デバッグ用の仮想時計」カードの「仮想時計に接続」を押す
3. 接続状態・デバイス情報・タイマー・メモ送信・設定・通知転送設定が全部動く（応答はインメモリ）
4. 「電池イベントを発生」「メディア操作イベントを発生」ボタンで時計→アプリ方向のイベントも試せる

## 画面スクリーンショット

```sh
./gradlew :app:recordRoborazziDebug   # build/outputs/roborazzi/*.png
```

## 推測している部分（`// TODO(hw): 実機で確認`）

- CRC-16 のバイトオーダー（little endian と仮定）
- フラグメントの MTU 計算（ATT ヘッダ 3 バイトを引いている）
- ボタン割り当ての Action 名一覧（`SettingsKeys.BUTTON_ACTIONS`）
- ボンディングの流れ（createBond の Numeric Comparison）
- 常駐通知を転送対象外にしている判断
