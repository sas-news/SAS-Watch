# Host tests (GoogleTest)。core/CMakeLists.txt から add_subdirectory される。

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE)
# gtest を g++/C++17 で。
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# テスト側は例外あり (GoogleTest が必要とする)。core のライブラリは
# -fno-exceptions でビルド済みだがリンクはできる。
add_executable(watch_core_tests
    test_core.cpp
    test_features.cpp
    test_protocol.cpp)

target_include_directories(watch_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(watch_core_tests PRIVATE watch_core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(watch_core_tests)
