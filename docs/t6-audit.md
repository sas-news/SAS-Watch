# T6 監査: 色・レイアウトの統一感 (監査のみ・コード変更なし)

対象スクショ: `/tmp/shots/t6/` (standard 全画面 + light 節、45枚、
safe_area_report 0 件)。ルールは roadmap.md T6 節。

## ルール1: primary は「主操作」と「ON/選択中」だけ。1画面に主操作は1つ

| # | 画面 | 違反内容 | コード | 直し方案 | スクショ |
|---|---|---|---|---|---|
| 1 | 全画面 (ヘッダ) | 戻るピルの「‹」シェブロンが primary (飾りグリフに強調色) | components.cpp:454 | `t.text_dim` に (行末 chevron の edge と合わせる手もある) | 11_settings.png |
| 2 | 設定の選択ビュー | back_row の「‹」も同じく primary | components.cpp:705 | 同上 | 30_settings_faces.png |
| 3 | 設定 (スライダー) | 値ラベル (50 等) が primary。値表示に強調色は主操作ではない | components.cpp:875 | `t.text` または `t.text_dim` | 11_settings.png |
| 4 | applist / quick | アプリアイコングリフが primary (T7 で導入)。タイル自体が主操作なので許容とも読めるが、厳密には「飾り」 | components.cpp:743 | 要判断 (primary 維持 or text/edge) | 02_applist.png |
| 5 | メモ / エージェント | 録音レベルバーの INDICATOR が primary (進捗=強調状態としては許容) | memo:154, agent:151 | 要判断 (primary 維持 or accent) | 09_memo_list.png |
| 6 | 歩数 | 目標進捗バーが primary | steps:81 | 同上 | 07_steps.png |
| 7 | タイマー | 残量リング (arc INDICATOR) が primary | timer:183 | 同上 (「主操作」ではないが進行状態の強調) | 04_timer_setup.png |
| 8 | OTA | 進捗バーが primary | ota:65 | 同上 | 50_ota.png 相当 (shot 名は一覧参照) |
| 9 | メディア | 音符グリフが primary | media:76 | アルバムアート無し時のフォールバック装飾 → `t.text_dim` か icon tile 化 | 28_media.png 相当 |

1画面1主操作は概ね遵守 (録音開始/再生/開始/＋1 が各画面の唯一の primary ボタン)。

## ルール2: danger は破壊的操作だけ

適合: メモ「削除」(confirm 付き・T14)、通知「すべて消す」(同)、
カウンター「リセット」(normal ボタンだが confirm 付き)。
違反なし。danger 色の文字/背景が他の用途に使われている箇所は見つからなかった。

## ルール3: 本文=text / 補足=text_dim 以外の文字色

| # | 画面 | 違反内容 | コード | 直し方案 | スクショ |
|---|---|---|---|---|---|
| 10 | カウンター | 数字が `t.accent` | counter:35 | `t.text` (数値ハイライトとして accent を残すなら要判断) | 08_counter.png |
| 11 | 歩数 | 歩数値が `t.accent` | steps:64 | 同上 | 07_steps.png |
| 12 | メディア | 状態表示 (再生中 等) が `t.accent` | media:86 | `t.text_dim` | media 系 shot |
| 13 | タイマー終了アラート | 「TIMER」タグが `t.primary2` | shell:528 | `t.text_dim` | 14_timer_alert.png |
| 14 | アラームアラート | 「アラーム」見出しが `t.accent` | shell:619 | `t.text` | 41_alarm_alert.png |
| 15 | パスキー | 6桁コードが `t.accent` | shell:747 | `t.text` (確認コードは強調してよいなら要判断) | 13_passkey.png |

※「数値ハイライトとして accent」は Theme のコメント意図とは合うが
T6 ルールの字義 (本文/補足のみ) とはずれる。10/11/15 は
「大きい数値には accent を許す」例外をルール側に足す判断もあり。

## ルール4: 余白は space の倍数 (8/16/24/32)

| # | 箇所 | 違反内容 | コード | 直し方案 |
|---|---|---|---|---|
| 16 | カード | `pad_all=12` | components.cpp:617 | 8 or 16 |
| 17 | コンテンツ列 | `pad_row=12` | components.cpp:480 | 8 or 16 |
| 18 | ヘッダ | `pad_column=12` | components.cpp:429 | 8 |
| 19 | 戻るピル | `pad_left=12`, `pad_column=4` | components.cpp:448,450 | 8/8 (見た目微調整) |
| 20 | 行内 sub テキスト | `pad_row=-14` (重ね書き) | components.cpp:652 | 意図的な詰めか確認 → 0 or -8 |
| (例外) | ヘッダ pad_top=26 / pad_right=42 | 角 R100 安全域由来の非倍数 | components.cpp:427,425 | 角帯回避のため意図値として維持 (ルールに「角由来は例外」を追記案) |
| (例外) | アラート系の絶対座標 (-100/-28, toast y=36 等) | 絶対配置のオフセット | shell.cpp:551,560, 他 | 同上・角帯ベースの値 |

## ルール5: ヘッダの形 (タイトル左寄せ・戻るピル・右 mascot)

- 全リスト画面は `c::header(scr, title, true)` で統一 ✓
- Home は意図的にヘッダ無し (文字盤) — 例外として妥当
- アラート/パスキー/confirm は layer_top のモーダル — 例外
- **21. 設定の選択ビュー (face/font picker)** はヘッダではなく
  `back_row` (リスト内の ‹戻る行) を使う — settings.cpp:66。
  形が他画面と違うが「画面内サブビュー」なので要判断
  (ヘッダ化するか、サブビューはこの形で統一するか)。

## 集計

- 確実な違反 (ルールが一意): 1,2,3,6(提案あり),9,12,13,14,16-19 → **14件**
- 要判断 (意味が変わる/例外化の判断): 4,5,6,7,8,10,11,15,20,21 → **10件**
- 適合確認済み: danger の用途・1画面1主操作・ヘッダ基本形

承認待ち: 上記の直し方案で直してよいか。
