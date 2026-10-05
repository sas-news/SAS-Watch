# core/ (ハード非依存 C++17) を ESP-IDF コンポーネントとして取り込む。
# ソース一覧は core/sources.cmake が正 (core/README.md 参照)。
# IDF のコンポーネント読み込みは CMAKE_CURRENT_SOURCE_DIR が正しく指さない
# フェーズがあるので COMPONENT_DIR を使う (IDF が必ず定義する)。
include(${COMPONENT_DIR}/../../../core/sources.cmake)

idf_component_register(SRCS ${WATCH_CORE_SOURCES}
                     INCLUDE_DIRS ${WATCH_CORE_INCLUDE_DIRS})

# core のルール (core/README.md): 例外・RTTI なし。
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
