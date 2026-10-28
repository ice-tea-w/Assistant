cmake_minimum_required(VERSION 3.20)
project(Assistant VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)      # Qt MOC 自动处理
set(CMAKE_AUTORCC ON)      # Qt 资源文件自动处理（QML 文件通过 .qrc 打包）
# 强制 MSVC 以 UTF-8 编码读取源文件
if(MSVC)
    add_compile_options("/utf-8")
endif()

# ---- 查找 Qt6（QML 方案） ----
find_package(Qt6 REQUIRED COMPONENTS
    Core Quick QuickControls2 Network Widgets Sql
)

# ---- 查找其他依赖 ----
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
# 插件市场：zip 解压（Qt6 没有公开的 zip API，QZipReader 是私有头）
find_package(miniz CONFIG REQUIRED)
# find_package(SQLite3 REQUIRED)    # Phase 2: uncomment when adding HistoryDatabase
# find_package(GTest REQUIRED)

# ---- 源文件 ----
file(GLOB_RECURSE SOURCES src/*.cpp)
file(GLOB_RECURSE HEADERS include/*.h)

# ---- 主程序 ----
add_executable(${PROJECT_NAME}
    ${SOURCES}
    ${HEADERS}
    resources/resources.qrc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# 版本号单一来源：project(VERSION) → 编译期宏 → QCoreApplication::applicationVersion()
# → QML 的 appVersion context property（避免 QML 里再写死一处 "v0.1.0"）
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Widgets
    Qt6::Sql
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    miniz::miniz
)

# AppIndexer uses the Windows Shell Link COM interface (IShellLink, SHGetKnownFolderPath)
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32 ole32)
endif()

# ---- Qt 部署 ----
# 开发阶段：在 exe 旁生成 qt.conf，指向 vcpkg 的 Qt6 安装目录。
# qt.conf 负责：platforms/、qml/ 的路径（Qt 内部查找）
# 但同时需要 vcpkg/bin 在 PATH 中（Windows DLL 加载器查找）

# 1. 生成 qt.conf（POST_BUILD，build 阶段执行）
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/write_qt_conf.cmake"
"file(WRITE \"\${OUT_FILE}\" \"[Paths]\\n\")
file(APPEND \"\${OUT_FILE}\" \"Prefix = \${QT_PREFIX}\\n\")
file(APPEND \"\${OUT_FILE}\" \"Plugins = Qt6/plugins\\n\")
file(APPEND \"\${OUT_FILE}\" \"QmlImports = Qt6/qml\\n\")
")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D "OUT_FILE=$<TARGET_FILE_DIR:${PROJECT_NAME}>/qt.conf"
        -D "QT_PREFIX=$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}>"
        -P "${CMAKE_CURRENT_BINARY_DIR}/write_qt_conf.cmake"
    COMMENT "Generating qt.conf (vcpkg Qt6)"
)

# 2. 生成 run.bat（POST_BUILD，build 阶段执行，generator expression 可用）
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/write_run_bat.cmake"
"file(WRITE \"\${OUT_FILE}\" \"@echo off\\n\")
file(APPEND \"\${OUT_FILE}\" \"set \\\"PATH=\${QT_PATH}/bin;%PATH%\\\"\\n\")
file(APPEND \"\${OUT_FILE}\" \"cd /d \\\"\${EXE_DIR}\\\"\\n\")
file(APPEND \"\${OUT_FILE}\" \"start \\\"\\\" \\\"\${EXE_NAME}\\\"\\n\")
")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -D "OUT_FILE=$<TARGET_FILE_DIR:${PROJECT_NAME}>/run.bat"
        -D "QT_PATH=$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}>"
        -D "EXE_DIR=$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        -D "EXE_NAME=$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        -P "${CMAKE_CURRENT_BINARY_DIR}/write_run_bat.cmake"
    COMMENT "Generating run.bat"
)

# 发布阶段（打包时替换以上所有部署相关代码）：
# qt_generate_deploy_app_script(
#     TARGET ${PROJECT_NAME}
#     OUTPUT_SCRIPT deploy_script
#     NO_UNSUPPORTED_PLATFORM_ERROR
# )
# install(SCRIPT ${deploy_script})

# ---- 测试（Phase 1 暂时禁用，编写测试时取消注释） ----
# enable_testing()
# add_executable(assistant_tests
#     tests/test_main.cpp
# )
# target_link_libraries(assistant_tests PRIVATE
#     GTest::gtest_main
# )
# include(GoogleTest)
# gtest_discover_tests(assistant_tests)
