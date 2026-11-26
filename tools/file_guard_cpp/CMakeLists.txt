cmake_minimum_required(VERSION 3.20)
project(file_guard_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(fg_core STATIC
    src/util.cpp
    src/crypto.cpp
    src/scan.cpp
    src/audit.cpp
    src/config.cpp
    src/vault.cpp
    src/backup.cpp
    src/watch.cpp
    src/service.cpp
    src/integrity.cpp
    src/zipx.cpp
)
target_include_directories(fg_core PUBLIC include)
target_link_libraries(fg_core PUBLIC bcrypt crypt32 ws2_32 advapi32 shell32)
if(MSVC)
    target_compile_options(fg_core PRIVATE /utf-8 /W3)
endif()

add_executable(file_guard src/main.cpp)
target_link_libraries(file_guard PRIVATE fg_core)
if(MSVC)
    target_compile_options(file_guard PRIVATE /utf-8 /W3)
endif()

enable_testing()
add_executable(fg_tests tests/test_core.cpp)
target_link_libraries(fg_tests PRIVATE fg_core)
if(MSVC)
    target_compile_options(fg_tests PRIVATE /utf-8 /W3)
endif()
add_test(NAME fg_tests COMMAND fg_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
