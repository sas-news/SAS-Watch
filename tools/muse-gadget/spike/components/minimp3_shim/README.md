# minimp3_shim (T49 spike)

B-min は返答を字幕で出すだけで音声再生をしないため、SDK の `components/minimp3`
(19,510B, CC0) をこのヘッダだけのシムに置き換えている。`muse_chat_session.cpp` は
無改変でコンパイルできる。

将来 TTS を足して MP3 を鳴らす場合は、このディレクトリを消して SDK の
`esp32/components/minimp3/` を `components/minimp3/` にコピーし、`muse_chat` の
`REQUIRES` を `minimp3_shim` → `minimp3` に変える。
