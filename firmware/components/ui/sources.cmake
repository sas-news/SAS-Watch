# ui の platform 非依存ソース一覧。sim/CMakeLists.txt も同じ物を使う。
set(WATCH_UI_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/theme.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/theme_manager.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/components.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/shell.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/registry.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/home.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/quick.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/applist.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/timer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/stopwatch.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/counter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/memo.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/alarm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/notifications.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/media.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/settings.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/ota.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/screens/powermenu.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/fonts/font_jp_20.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fonts/font_jp_26.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fonts/font_digits_96.c
    ${CMAKE_CURRENT_LIST_DIR}/src/fonts/font_digits_56.c
)

set(WATCH_UI_INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/include)
