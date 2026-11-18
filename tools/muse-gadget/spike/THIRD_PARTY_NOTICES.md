# Third-party notices — tools/muse-gadget/spike

このディレクトリのうち、下記のファイルは Meta の Muse Gadget SDK から
**無改変でコピー**したものです (Apache License 2.0)。

- 上流: https://github.com/facebookincubator/muse-gadget-sdk
- pinned commit: `ec976f0537db594b0947a2d1a78926575f97a895` (2026-11-18)
- 対象:
  - `components/noise_core/` 全体
  - `components/muse_chat/src/` (`muse_chat.c`, `muse_chat_session.cpp`,
    `muse_chat_text.c`, `muse_account_api.c`, `muse_text.c`)
  - `components/muse_api/include/` のヘッダ
- ライセンス全文: `LICENSE` (Apache License 2.0)

上記以外 (`components/muse_stubs/`, `components/minimp3_shim/`, `main/`,
ビルドスクリプト) は SAS-Watch プロジェクトが本スパイク用に書いたものです。
`minimp3_shim` は「音声再生をしない B-min 構成」のための最小ヘッダで、
minimp3 本体は含みません (TTS を足す場合は上流の CC0-1.0 版を利用)。

このスパイクは SAS-Watch 本体ファームには組み込まれていません。
