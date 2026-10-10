cmake_minimum_required(VERSION 3.16)
project(watch_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ESP-IDF から取り込まれるときはテストを作らない。
# (firmware 側は sources.cmake だけを参照するので、このファイル自体は
#  ESP-IDF からは読まれない想定)
if(NOT DEFINED WATCH_CORE_BUILD_TESTS)
  if(DEFINED IDF_TARGET OR DEFINED ESP_PLATFORM)
    set(WATCH_CORE_BUILD_TESTS OFF)
  else()
    set(WATCH_CORE_BUILD_TESTS ON)
  endif()
endif()
option(WATCH_CORE_BUILD_TESTS "Build host tests" ${WATCH_CORE_BUILD_TESTS})

include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)
# アプリの有効/無効とソース収集は apps/cmake/apps.cmake が正。
include(${CMAKE_CURRENT_SOURCE_DIR}/../apps/cmake/apps.cmake)

add_library(watch_core STATIC ${WATCH_CORE_SOURCES} ${SAS_APP_CORE_SOURCES})
target_include_directories(watch_core PUBLIC ${WATCH_CORE_INCLUDE_DIRS})
target_compile_definitions(watch_core PUBLIC ${SAS_APP_DEFINES})
target_compile_options(watch_core PRIVATE
    -Wall -Wextra -Werror -fno-exceptions -fno-rtti)

if(WATCH_CORE_BUILD_TESTS)
  enable_testing()
  # 既存テストは全アプリを前提に書かれている (無効アプリのシンボルを直接
  # 参照する) ので、絞ったビルドではテスト自体を作らない。
  if(SAS_APP_ALL_ENABLED)
    add_subdirectory(tests)
  else()
    message(STATUS "watch_core: アプリ絞り込み中のため core/tests はビルドしない")
  endif()
endif()
