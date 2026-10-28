# Standalone test project for the plugin core.
#
# Why a separate CMake project instead of the commented-out GTest block in the
# root CMakeLists.txt:
#   * the root project builds the GUI app; adding a test target there means every
#     test edit reconfigures the app build;
#   * GTest is not vendored yet, and these checks need no framework at all.
#
# Build (from the repo root, vcpkg toolchain required so Qt is found):
#
#   cmake -S tests -B ../_plugin-tests-build -G "Visual Studio 17 2022" -A x64 \
#         -DCMAKE_TOOLCHAIN_FILE=D:/STUDY/vcpkg/scripts/buildsystems/vcpkg.cmake \
#         -DVCPKG_TARGET_TRIPLET=x64-windows
#   cmake --build ../_plugin-tests-build --config Release
#   ../_plugin-tests-build/Release/assistant_core_tests.exe
#
cmake_minimum_required(VERSION 3.20)

project(assistant_core_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PluginHost / PluginLoader declare Q_OBJECT, so their moc output must be generated.
# The main project gets this from its own CMakeLists; the test project needs it too.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_RELAXED_MODE ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(miniz CONFIG REQUIRED)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(assistant_core_tests
    plugin_core_tests.cpp
    # Q_OBJECT headers must be listed so AUTOMOC can find them; they do not sit
    # next to their .cpp (public headers live under include/), so the usual
    # same-basename association does not apply.
    ${REPO_ROOT}/include/core/plugin/PluginHost.h
    ${REPO_ROOT}/include/core/plugin/PluginLoader.h
    ${REPO_ROOT}/include/core/plugin/PluginManager.h
    ${REPO_ROOT}/include/core/plugin/PluginInstaller.h
    ${REPO_ROOT}/src/core/plugin/SemVer.cpp
    ${REPO_ROOT}/src/core/plugin/PluginManifest.cpp
    ${REPO_ROOT}/src/core/plugin/PluginRegistry.cpp
    ${REPO_ROOT}/src/core/plugin/PluginHost.cpp
    ${REPO_ROOT}/src/core/plugin/PluginLoader.cpp
    ${REPO_ROOT}/src/core/plugin/PluginManager.cpp
    ${REPO_ROOT}/src/core/plugin/PluginSpec.cpp
    ${REPO_ROOT}/src/core/plugin/PluginArchive.cpp
    ${REPO_ROOT}/src/core/plugin/PluginInstaller.cpp
    ${REPO_ROOT}/src/core/Logger.cpp
)

target_include_directories(assistant_core_tests PRIVATE
    ${REPO_ROOT}/include
)

target_link_libraries(assistant_core_tests PRIVATE
    Qt6::Core
    Qt6::Gui      # QDesktopServices (PluginHost::openPath)
    Qt6::Qml      # QQmlEngine / QQmlComponent (PluginLoader)
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    miniz::miniz
)

# The shipped app locates QML modules through the generated qt.conf. The test
# executable has no qt.conf, so QQmlEngine would report `module "QtQml" is not
# installed`. Bake the import path in at configure time and let main() put it into
# QML2_IMPORT_PATH before any engine is created.
if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(PLUGIN_QML_IMPORT_DIR
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/qml")
    if (EXISTS "${PLUGIN_QML_IMPORT_DIR}")
        target_compile_definitions(assistant_core_tests PRIVATE
            "PLUGIN_TEST_QML_IMPORT_PATH=\"${PLUGIN_QML_IMPORT_DIR}\"")
        message(STATUS "QML import path for tests: ${PLUGIN_QML_IMPORT_DIR}")
    else()
        message(WARNING "QML import dir not found: ${PLUGIN_QML_IMPORT_DIR}")
    endif()
endif()

# The sources contain UTF-8 Chinese string literals (manifest error messages).
if (MSVC)
    target_compile_options(assistant_core_tests PRIVATE /utf-8)
endif()

# Run the tests by building the RUN_TESTS target, or just execute the binary.
enable_testing()
add_test(NAME plugin_core COMMAND assistant_core_tests)
