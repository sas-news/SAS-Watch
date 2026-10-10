# core/ (ハード非依存 C++17) を ESP-IDF コンポーネントとして取り込む。
# ソース一覧は core/sources.cmake が正 (core/README.md 参照)。
# IDF のコンポーネント読み込みは CMAKE_CURRENT_SOURCE_DIR が正しく指さない
# フェーズがあるので COMPONENT_DIR を使う (IDF が必ず定義する)。
include(${COMPONENT_DIR}/../../../core/sources.cmake)
# アプリ (apps/<id>/) の有効/無効とソース収集。
include(${COMPONENT_DIR}/../../../apps/cmake/apps.cmake)

idf_component_register(SRCS ${WATCH_CORE_SOURCES} ${SAS_APP_CORE_SOURCES}
                     INCLUDE_DIRS ${WATCH_CORE_INCLUDE_DIRS})

# SAS_APP_<ID>=0/1 を全アプリ分定義 (依存する他コンポーネントへ伝播)。
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${SAS_APP_DEFINES})

# core のルール (core/README.md): 例外・RTTI なし。
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
