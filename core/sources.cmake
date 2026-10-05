# watch_core のソース一覧。
# PC の CMakeLists.txt と firmware/components/watch_core の両方から使う。
# ESP-IDF 側は:
#   include(<repo>/core/sources.cmake)
#   idf_component_register(SRCS ${WATCH_CORE_SOURCES}
#                        INCLUDE_DIRS ${WATCH_CORE_INCLUDE_DIRS})
# だけで取り込める。

set(WATCH_CORE_DIR ${CMAKE_CURRENT_LIST_DIR})

set(WATCH_CORE_INCLUDE_DIRS
    ${WATCH_CORE_DIR}/include)

set(WATCH_CORE_SOURCES
    ${WATCH_CORE_DIR}/src/platform.cpp
    ${WATCH_CORE_DIR}/src/adpcm.cpp
    ${WATCH_CORE_DIR}/src/event_bus.cpp
    ${WATCH_CORE_DIR}/src/navigation.cpp
    ${WATCH_CORE_DIR}/src/power.cpp
    ${WATCH_CORE_DIR}/src/input_mapper.cpp
    ${WATCH_CORE_DIR}/src/feature.cpp
    ${WATCH_CORE_DIR}/src/settings.cpp
    ${WATCH_CORE_DIR}/src/runtime.cpp
    ${WATCH_CORE_DIR}/features/clock/clock.cpp
    ${WATCH_CORE_DIR}/features/timer/timer.cpp
    ${WATCH_CORE_DIR}/features/stopwatch/stopwatch.cpp
    ${WATCH_CORE_DIR}/features/counter/counter.cpp
    ${WATCH_CORE_DIR}/features/memo/memo.cpp
    ${WATCH_CORE_DIR}/features/registry.cpp
    ${WATCH_CORE_DIR}/protocol/crc16.cpp
    ${WATCH_CORE_DIR}/protocol/cbor.cpp
    ${WATCH_CORE_DIR}/protocol/frame.cpp
    ${WATCH_CORE_DIR}/protocol/sha256.cpp
    ${WATCH_CORE_DIR}/protocol/bulk.cpp
    ${WATCH_CORE_DIR}/protocol/dispatch.cpp
    ${WATCH_CORE_DIR}/theme/manifest.cpp
    ${WATCH_CORE_DIR}/theme/package.cpp)
