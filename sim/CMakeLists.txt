# sim/ — ホストシミュレータ。ui コンポーネントと core を Linux でビルドし、
# LVGL のメモリ上ディスプレイ (410x502) に描画して PNG を書き出す。
#   cmake -S sim -B sim/build && cmake --build sim/build
#   sim/build/snapshot --out docs/screenshots/
cmake_minimum_required(VERSION 3.16)
project(sas_watch_sim CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# LVGL は firmware と同じバージョン (dependencies.lock の lvgl 9.5.0)。
# Espressif レジストリの zip は GitHub より軽量に取れる。
FetchContent_Declare(
    lvgl
    URL https://components-file.espressif.com/components/lvgl/lvgl/9.5.0/lvgl__lvgl-v9.5.0.zip
)
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE PATH "" FORCE)
FetchContent_MakeAvailable(lvgl)

# core は sources.cmake が正 (firmware の watch_core と同じ取り込み方)。
include(${CMAKE_CURRENT_SOURCE_DIR}/../core/sources.cmake)
add_library(watch_core STATIC ${WATCH_CORE_SOURCES})
target_include_directories(watch_core PUBLIC ${WATCH_CORE_INCLUDE_DIRS})
target_compile_options(watch_core PRIVATE -fno-exceptions -fno-rtti)

# ui の platform 非依存ソース (firmware と共有)。
include(${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ui/sources.cmake)
add_library(watch_ui STATIC ${WATCH_UI_SOURCES})
target_include_directories(watch_ui PUBLIC ${WATCH_UI_INCLUDE_DIRS})
target_include_directories(watch_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/ui/src)
target_link_libraries(watch_ui PUBLIC lvgl watch_core)
target_compile_options(watch_ui PRIVATE -fno-exceptions -fno-rtti)

add_executable(snapshot
    snapshot.cpp
    sim_platform.cpp
    sim_port.cpp
    png.cpp
)
target_include_directories(snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(snapshot PRIVATE watch_ui watch_core lvgl z)
target_compile_options(snapshot PRIVATE -fno-exceptions -fno-rtti)
