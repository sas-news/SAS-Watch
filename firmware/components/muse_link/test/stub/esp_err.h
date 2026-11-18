/* ホストテスト用の esp_err.h スタブ (T55)。
 * muse_link.h は公開 API で esp_err_t を使うが、純 C テストには IDF が無い。
 * esp_err_t の型だけ与えて、純粋ロジック (session_logic.c) をホストで検証する。*/
#pragma once
typedef int esp_err_t;
