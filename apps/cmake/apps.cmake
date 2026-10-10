# apps.cmake — アプリの有効/無効を読み取り、ソースと define を各ビルドへ出す。
#   apps/enabled.txt        : 書き込むアプリを1行1id。先頭 # はコメント。
#   -DSAS_APPS_ENABLED="timer;steps" : enabled.txt の代わりに使う (検証用)。
#
# include すると呼び出し側スコープに以下が定義される (全て絶対パス)。
#   SAS_APP_ENABLED_IDS  : 有効なアプリ id の一覧
#   SAS_APP_KNOWN_IDS    : apps/ に存在する全アプリ id の一覧
#   SAS_APP_ALL_ENABLED  : 全アプリが有効なら TRUE
#   SAS_APP_CORE_SOURCES : apps/<id>/feature/*.cpp (有効なものだけ)
#   SAS_APP_UI_SOURCES   : apps/<id>/screen.cpp + apps/<id>/screen/*.cpp
#   SAS_APP_TEST_SOURCES : apps/<id>/test_*.cpp
#   SAS_APP_DEFINES      : 全アプリの SAS_APP_<ID>=1/0 (0 も必ず定義するので
#                          #if と constexpr 算術の両方に使える)
#
# 呼び出し側は sources に各 *_SOURCES を append し、
#   target_compile_definitions(<target> PUBLIC ${SAS_APP_DEFINES})
# するだけでよい。core/sources.cmake と同じく CMAKE_CURRENT_LIST_DIR から
# apps/ の絶対パスを引く (IDF の requirements フェーズで
# CMAKE_CURRENT_SOURCE_DIR が build dir を指す罠の回避)。

get_filename_component(SAS_APPS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SAS_APPS_ENABLED_FILE ${SAS_APPS_DIR}/enabled.txt)

# enabled.txt を編集しただけでも configure をやり直させる。
# ESP-IDF の requirements フェーズ (スクリプトモード) では DIRECTORY プロパティを
# 書けないので、その時だけスキップする。
if(NOT CMAKE_SCRIPT_MODE_FILE)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${SAS_APPS_ENABLED_FILE})
endif()

# --- 有効 id 一覧: apps/ 直下で feature/ or screen を持つディレクトリ --------
file(GLOB _sas_app_entries LIST_DIRECTORIES true ${SAS_APPS_DIR}/*)
set(SAS_APP_KNOWN_IDS "")
foreach(_entry IN LISTS _sas_app_entries)
  get_filename_component(_id ${_entry} NAME)
  if(IS_DIRECTORY ${_entry} AND NOT _id STREQUAL "cmake")
    if(EXISTS ${_entry}/feature OR EXISTS ${_entry}/screen.cpp OR
       EXISTS ${_entry}/screen)
      list(APPEND SAS_APP_KNOWN_IDS ${_id})
    endif()
  endif()
endforeach()
list(SORT SAS_APP_KNOWN_IDS)

# --- 有効化リスト: -D 上書き か enabled.txt --------------------------------
if(DEFINED SAS_APPS_ENABLED)
  set(_sas_enabled ${SAS_APPS_ENABLED})
else()
  if(NOT EXISTS ${SAS_APPS_ENABLED_FILE})
    message(FATAL_ERROR "apps/enabled.txt がありません: ${SAS_APPS_ENABLED_FILE}")
  endif()
  set(_sas_enabled "")
  # コメントに日本語を書けるよう file(STRINGS) は使わず自分で行分割する
  # (file(STRINGS) はマルチバイト文字を含む行を壊すことがある)。
  file(READ ${SAS_APPS_ENABLED_FILE} _sas_enabled_txt)
  string(REPLACE "\r\n" "\n" _sas_enabled_txt "${_sas_enabled_txt}")
  string(REPLACE "\r" "\n" _sas_enabled_txt "${_sas_enabled_txt}")
  string(REGEX REPLACE ";" "\\\\;" _sas_enabled_txt "${_sas_enabled_txt}")
  string(REPLACE "\n" ";" _sas_enabled_lines "${_sas_enabled_txt}")
  foreach(_line IN LISTS _sas_enabled_lines)
    string(STRIP "${_line}" _line)
    if(_line AND NOT _line MATCHES "^#")
      list(APPEND _sas_enabled "${_line}")
    endif()
  endforeach()
endif()
if(_sas_enabled)
  list(REMOVE_DUPLICATES _sas_enabled)
endif()

# --- 検証: 未知の id は有効 id 一覧を出して落とす ---------------------------
foreach(_id IN LISTS _sas_enabled)
  if(NOT _id IN_LIST SAS_APP_KNOWN_IDS)
    string(REPLACE ";" ", " _known "${SAS_APP_KNOWN_IDS}")
    message(FATAL_ERROR
      "apps: 不明なアプリ id '${_id}' (enabled.txt)。有効な id: ${_known}")
  endif()
endforeach()

set(SAS_APP_ENABLED_IDS ${_sas_enabled})
set(SAS_APP_ALL_ENABLED TRUE)
foreach(_id IN LISTS SAS_APP_KNOWN_IDS)
  if(NOT _id IN_LIST SAS_APP_ENABLED_IDS)
    set(SAS_APP_ALL_ENABLED FALSE)
  endif()
endforeach()

# --- ソース収集 + define 生成 ----------------------------------------------
set(SAS_APP_CORE_SOURCES "")
set(SAS_APP_UI_SOURCES "")
set(SAS_APP_TEST_SOURCES "")
set(SAS_APP_DEFINES "")

foreach(_id IN LISTS SAS_APP_KNOWN_IDS)
  string(TOUPPER ${_id} _ID)
  if(_id IN_LIST SAS_APP_ENABLED_IDS)
    list(APPEND SAS_APP_DEFINES SAS_APP_${_ID}=1)
    # CONFIGURE_DEPENDS はスクリプトモード (IDF requirements フェーズ) で
    # エラーになるので付けない。
    file(GLOB _feat ${SAS_APPS_DIR}/${_id}/feature/*.cpp)
    list(APPEND SAS_APP_CORE_SOURCES ${_feat})
    if(EXISTS ${SAS_APPS_DIR}/${_id}/screen.cpp)
      list(APPEND SAS_APP_UI_SOURCES ${SAS_APPS_DIR}/${_id}/screen.cpp)
    endif()
    file(GLOB _scr ${SAS_APPS_DIR}/${_id}/screen/*.cpp)
    list(APPEND SAS_APP_UI_SOURCES ${_scr})
    file(GLOB _tst ${SAS_APPS_DIR}/${_id}/test_*.cpp)
    list(APPEND SAS_APP_TEST_SOURCES ${_tst})
  else()
    list(APPEND SAS_APP_DEFINES SAS_APP_${_ID}=0)
  endif()
endforeach()

message(STATUS "apps: enabled = ${SAS_APP_ENABLED_IDS}")
