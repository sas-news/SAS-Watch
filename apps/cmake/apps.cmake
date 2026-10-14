# apps.cmake v2 — アプリの自動発見 + 登録表生成 (T19)。
#
# 発見対象:
#   ${SAS_APPS_DIR}/<id>/app.cmake   (apps/ 直下。"_" 始まりは除外 = 雛形)
#   WATCH_EXTRA_APP_DIRS 内の <id>/app.cmake  (; 区切りのコンテナdir一覧)
#
# 各 app.cmake は次の変数を set する:
#   APP_ID           アプリ id ([a-z0-9-]+)。省略時は dir 名。
#   APP_NAME         applist の表示名 (日本語)
#   APP_ENABLED      0/1。省略時 1。-DSAS_APPS_ENABLED="a;b" で一括上書き可
#   APP_FEATURE_SYM  FeatureDescriptor の完全修飾名 (watch::features::kXxx)
#   APP_FEATURE_HDR  上記を宣言するヘッダ (絶対 or core/include からの相対)
#   APP_ROUTE        watch::Route::Timer など。省略時 App0 から自動採番
#   APP_SCREEN_SYM   "ui::kXxxScreen" (ScreenOps 実体)。画面無しは省略
#   APP_ICON_CP      font_icon_36 のコードポイント (例 0xE425)
#   APP_ICON_BG      アイコンタイル色 (例 0xFF8A3D)
#   APP_SUB          applist サブ行の関数 (const char* fn(), ui namespace)。任意
#   APP_ORDER        applist の並び (既定 50。小さいほど上)
#
# include すると呼び出しスコープに:
#   SAS_APP_ENABLED_IDS / SAS_APP_KNOWN_IDS / SAS_APP_ALL_ENABLED
#   SAS_APP_CORE_SOURCES : <app>/feature/*.cpp (有効のみ)
#   SAS_APP_UI_SOURCES   : <app>/screen.cpp + <app>/screen/*.cpp + 生成 app_ui.cpp
#   SAS_APP_TEST_SOURCES : <app>/test_*.cpp
#   SAS_APP_DEFINES      : SAS_APP_<ID>=1/0 (全アプリ分)
#   SAS_APP_GEN_DIR      : 生成 dir (${CMAKE_BINARY_DIR}/sas_apps_gen)
#                          app_registry.cpp (core) / app_ui.hpp + app_ui.cpp (ui)

get_filename_component(SAS_APPS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(SAS_REPO_ROOT "${SAS_APPS_DIR}/.." ABSOLUTE)
set(SAS_APP_GEN_DIR ${CMAKE_BINARY_DIR}/sas_apps_gen)
file(MAKE_DIRECTORY ${SAS_APP_GEN_DIR})

# --- app.cmake の走査 -------------------------------------------------------
set(_sas_app_dirs "")
file(GLOB _sas_app_entries LIST_DIRECTORIES true ${SAS_APPS_DIR}/*)
foreach(_entry IN LISTS _sas_app_entries)
  if(IS_DIRECTORY ${_entry})
    get_filename_component(_d ${_entry} NAME)
    if(NOT _d MATCHES "^_" AND NOT _d STREQUAL "cmake" AND
       EXISTS ${_entry}/app.cmake)
      list(APPEND _sas_app_dirs ${_entry})
    endif()
  endif()
endforeach()

# 外部ディレクトリ (各要素は <appid>/ をぶら下げるコンテナ)
if(DEFINED WATCH_EXTRA_APP_DIRS AND NOT "${WATCH_EXTRA_APP_DIRS}" STREQUAL "")
  foreach(_extra IN LISTS WATCH_EXTRA_APP_DIRS)
    file(GLOB _extra_entries LIST_DIRECTORIES true ${_extra}/*)
    foreach(_entry IN LISTS _extra_entries)
      if(IS_DIRECTORY ${_entry} AND EXISTS ${_entry}/app.cmake)
        list(APPEND _sas_app_dirs ${_entry})
      endif()
    endforeach()
  endforeach()
endif()

set(_sas_app_vars APP_ID APP_NAME APP_ENABLED APP_FEATURE_SYM APP_FEATURE_HDR
    APP_ROUTE APP_SCREEN_SYM APP_ICON_CP APP_ICON_BG APP_SUB APP_ORDER)

# --- 各 app.cmake を読んでメタを集める --------------------------------------
set(SAS_APP_KNOWN_IDS "")
set(SAS_APP_ENABLED_IDS "")
set(_sas_reg_hdrs "")    # feature ヘッダの #include
set(_sas_reg_syms "")    # descriptor 参照
set(_sas_ui_externs "")  # screen/sub の extern 宣言
set(_sas_ui_cases "")    # screen_ops の case
set(_sas_ui_rows "")     # applist 行
set(_sas_dynamic 0)
set(SAS_APP_CORE_SOURCES "")
set(SAS_APP_UI_SOURCES "")
set(SAS_APP_TEST_SOURCES "")
set(SAS_APP_DEFINES "")

foreach(_dir IN LISTS _sas_app_dirs)
  foreach(_v IN LISTS _sas_app_vars)
    unset(${_v})
  endforeach()
  include(${_dir}/app.cmake)

  if(NOT DEFINED APP_ID)
    get_filename_component(APP_ID ${_dir} NAME)
  endif()
  if(NOT APP_ID MATCHES "^[a-z0-9-]+$")
    message(FATAL_ERROR "apps: 不正な id '${APP_ID}' (app.cmake @${_dir})")
  endif()
  if(NOT DEFINED APP_ENABLED)
    set(APP_ENABLED 1)
  endif()
  # -DSAS_APPS_ENABLED があれば「そこに入っているものだけ有効」
  if(DEFINED SAS_APPS_ENABLED)
    if(APP_ID IN_LIST SAS_APPS_ENABLED)
      set(APP_ENABLED 1)
    else()
      set(APP_ENABLED 0)
    endif()
  endif()
  if(NOT DEFINED APP_NAME)
    set(APP_NAME "${APP_ID}")
  endif()
  if(NOT DEFINED APP_ORDER)
    set(APP_ORDER 50)  # 50 台が既定。小さいほど applist の上
  endif()
  if(NOT DEFINED APP_ROUTE)
    # 外部アプリ用の動的 Route
    if(_sas_dynamic GREATER 7)
      message(FATAL_ERROR "apps: 動的 Route (App0-7) が足りない @${_dir}")
    endif()
    set(APP_ROUTE "watch::Route::App${_sas_dynamic}")
    math(EXPR _sas_dynamic "${_sas_dynamic} + 1")
  endif()
  if(NOT DEFINED APP_FEATURE_SYM OR NOT DEFINED APP_FEATURE_HDR)
    message(FATAL_ERROR "apps: ${APP_ID} は APP_FEATURE_SYM/APP_FEATURE_HDR が要る")
  endif()
  # ヘッダ解決: 絶対パス or core/include or アプリdir からの相対
  set(_fhdr ${APP_FEATURE_HDR})
  if(NOT IS_ABSOLUTE ${_fhdr})
    if(EXISTS ${SAS_REPO_ROOT}/core/include/${APP_FEATURE_HDR})
      set(_fhdr ${SAS_REPO_ROOT}/core/include/${APP_FEATURE_HDR})
    elseif(EXISTS ${_dir}/${APP_FEATURE_HDR})
      set(_fhdr ${_dir}/${APP_FEATURE_HDR})
    endif()
  endif()
  if(NOT EXISTS ${_fhdr})
    message(FATAL_ERROR "apps: ${APP_ID} の APP_FEATURE_HDR が無い: ${_fhdr}")
  endif()

  string(TOUPPER ${APP_ID} _ID)
  string(REPLACE "-" "_" _ID "${_ID}")

  list(APPEND SAS_APP_KNOWN_IDS ${APP_ID})
  if(APP_ENABLED)
    list(APPEND SAS_APP_DEFINES SAS_APP_${_ID}=1)
  else()
    list(APPEND SAS_APP_DEFINES SAS_APP_${_ID}=0)
  endif()

  if(APP_ENABLED)
    list(APPEND SAS_APP_ENABLED_IDS ${APP_ID})
    string(APPEND _sas_reg_syms "    &${APP_FEATURE_SYM},\n")
    string(APPEND _sas_reg_hdrs "#include \"${_fhdr}\"\n")
    if(DEFINED APP_SCREEN_SYM)
      string(REGEX REPLACE "^ui::" "" _ssym "${APP_SCREEN_SYM}")
      string(APPEND _sas_ui_externs
        "namespace ui { extern const ScreenOps ${_ssym}; }\n")
      string(APPEND _sas_ui_cases
        "    case ${APP_ROUTE}: return &${_ssym};\n")
    endif()
    if(DEFINED APP_SUB)
      string(APPEND _sas_ui_externs
        "namespace ui { const char* ${APP_SUB}(); }\n")
      set(_sub "${APP_SUB}")
    else()
      set(_sub "nullptr")
    endif()
    if(DEFINED APP_ICON_CP AND DEFINED APP_ICON_BG)
      string(REGEX REPLACE "^0[xX]" "" _cp_hex "${APP_ICON_CP}")
      list(APPEND _sas_ui_rows_sort "${APP_ORDER}@@  {\"${APP_ID}\", u8\"\\u${_cp_hex}\", ${APP_ICON_BG}, \"${APP_NAME}\", ${_sub}, ${APP_ROUTE}},\n")
    else()
      list(APPEND _sas_ui_rows_sort "${APP_ORDER}@@  {\"${APP_ID}\", nullptr, 0, \"${APP_NAME}\", ${_sub}, ${APP_ROUTE}},\n")
    endif()

    file(GLOB _feat ${_dir}/feature/*.cpp)
    list(APPEND SAS_APP_CORE_SOURCES ${_feat})
    if(EXISTS ${_dir}/screen.cpp)
      list(APPEND SAS_APP_UI_SOURCES ${_dir}/screen.cpp)
    endif()
    file(GLOB _scr ${_dir}/screen/*.cpp)
    list(APPEND SAS_APP_UI_SOURCES ${_scr})
    file(GLOB _tst ${_dir}/test_*.cpp)
    list(APPEND SAS_APP_TEST_SOURCES ${_tst})
  endif()
endforeach()

list(SORT SAS_APP_KNOWN_IDS)
set(SAS_APP_ALL_ENABLED TRUE)
foreach(_id IN LISTS SAS_APP_KNOWN_IDS)
  if(NOT _id IN_LIST SAS_APP_ENABLED_IDS)
    set(SAS_APP_ALL_ENABLED FALSE)
  endif()
endforeach()

# --- 生成: feature 登録表 (core) ---------------------------------------------
set(_reg_cpp "// GENERATED — apps/*/app.cmake と WATCH_EXTRA_APP_DIRS から自動生成。\n")
string(APPEND _reg_cpp "// 編集しない。再生成は cmake の configure 時。\n")
string(APPEND _reg_cpp "#include \"watch/feature.hpp\"\n")
string(APPEND _reg_cpp "#include \"${SAS_REPO_ROOT}/core/include/watch/features/clock.hpp\"\n")
string(APPEND _reg_cpp "${_sas_reg_hdrs}\n")
string(APPEND _reg_cpp "namespace watch {\nnamespace {\n")
string(APPEND _reg_cpp "const FeatureDescriptor* const kAppFeatures[] = {\n")
string(APPEND _reg_cpp "    &features::kClock,\n${_sas_reg_syms}};\n}\n")
string(APPEND _reg_cpp "const FeatureDescriptor* const* builtin_features() { return kAppFeatures; }\n")
string(APPEND _reg_cpp "size_t builtin_features_count() { return sizeof(kAppFeatures) / sizeof(kAppFeatures[0]); }\n")
string(APPEND _reg_cpp "}  // namespace watch\n")
file(WRITE ${SAS_APP_GEN_DIR}/app_registry.cpp "${_reg_cpp}")

# --- 生成: ui 側 (screen_ops 拡張 + applist 行) ------------------------------
set(_ui_hpp "// GENERATED — apps/*/app.cmake から自動生成。編集しない。\n")
string(APPEND _ui_hpp "#pragma once\n#include <cstddef>\n#include <cstdint>\n#include \"watch/navigation.hpp\"\n")
string(APPEND _ui_hpp "namespace ui {\nstruct ScreenOps;\n")
string(APPEND _ui_hpp "// app.cmake 由来の画面 → ScreenOps。無ければ nullptr。\n")
string(APPEND _ui_hpp "const ScreenOps* app_screen_ops(watch::Route r);\n")
string(APPEND _ui_hpp "struct SasAppRow {\n")
string(APPEND _ui_hpp "  const char* id;\n  const char* icon;\n  uint32_t icon_bg;\n")
string(APPEND _ui_hpp "  const char* name;\n  const char* (*sub)();\n  watch::Route route;\n};\n")
string(APPEND _ui_hpp "extern const SasAppRow kSasAppRows[];\n")
string(APPEND _ui_hpp "constexpr size_t kSasAppRowCount = ")
list(LENGTH SAS_APP_ENABLED_IDS _nrows)
string(APPEND _ui_hpp "${_nrows};\n}  // namespace ui\n")
file(WRITE ${SAS_APP_GEN_DIR}/app_ui.hpp "${_ui_hpp}")

set(_ui_cpp "// GENERATED — apps/*/app.cmake から自動生成。編集しない。\n")
get_filename_component(_screens_hpp
  "${SAS_REPO_ROOT}/firmware/components/ui/src/screens/screens.hpp" ABSOLUTE)
string(APPEND _ui_cpp "#include \"${_screens_hpp}\"\n#include \"app_ui.hpp\"\n\n")
string(APPEND _ui_cpp "${_sas_ui_externs}\n")
string(APPEND _ui_cpp "namespace ui {\n")
string(APPEND _ui_cpp "const ScreenOps* app_screen_ops(watch::Route r) {\n  switch (r) {\n")
string(APPEND _ui_cpp "${_sas_ui_cases}    default: return nullptr;\n  }\n}\n")
# applist 行は APP_ORDER 順 (同順は id で決着)
if(_sas_ui_rows_sort)
  list(SORT _sas_ui_rows_sort)
endif()
set(_sas_ui_rows "")
foreach(_r IN LISTS _sas_ui_rows_sort)
  string(REGEX REPLACE "^[0-9]+@@" "" _r "${_r}")
  string(APPEND _sas_ui_rows "${_r}")
endforeach()
string(APPEND _ui_cpp "const SasAppRow kSasAppRows[] = {\n${_sas_ui_rows}};\n")
string(APPEND _ui_cpp "}  // namespace ui\n")
file(WRITE ${SAS_APP_GEN_DIR}/app_ui.cpp "${_ui_cpp}")

# ui 側のソースには生成 app_ui.cpp も入れる
list(APPEND SAS_APP_UI_SOURCES ${SAS_APP_GEN_DIR}/app_ui.cpp)
# core 側には生成 app_registry.cpp (core/features/registry.cpp の代替)
list(APPEND SAS_APP_CORE_SOURCES ${SAS_APP_GEN_DIR}/app_registry.cpp)

# app_ui.cpp / applist が app_ui.hpp を見つけられるよう include dir を出す
set(SAS_APP_GEN_INCLUDE ${SAS_APP_GEN_DIR})

# --- 検証: -D 指定の未知 id は落とす -----------------------------------------
if(DEFINED SAS_APPS_ENABLED)
  foreach(_id IN LISTS SAS_APPS_ENABLED)
    if(NOT _id IN_LIST SAS_APP_KNOWN_IDS)
      string(REPLACE ";" ", " _known "${SAS_APP_KNOWN_IDS}")
      message(FATAL_ERROR
        "apps: 不明なアプリ id '${_id}'。有効な id: ${_known}")
    endif()
  endforeach()
endif()

message(STATUS "apps: enabled = ${SAS_APP_ENABLED_IDS}")
