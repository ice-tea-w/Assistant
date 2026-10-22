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
        -D "QT_PREFIX=$<IF:$<CONFIG:Debug>,C:/vcpkg/installed/x64-windows/debug,C:/vcpkg/installed/x64-windows>"
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
        -D "QT_PATH=$<IF:$<CONFIG:Debug>,C:/vcpkg/installed/x64-windows/debug,C:/vcpkg/installed/x64-windows>"
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

# ---- 测试 / 验收 ----
# A2 的 golden 对比（「同一候选集上 v2 与旧版查询结果逐条一致」）与 A3 的采集/持久化
# 验收（迁移、剪枝、差集收敛、扫描吞吐）共用同一份最小源集合。
# 只链 Core + Sql：验收工具不碰 GUI/QML，也不需要 ConfigManager。
# （`enable_testing()` + `bench_file_index` 仍是 A4 的事，见 v0.4-实施清单。）
set(ASSISTANT_INDEX_TEST_SOURCES
    src/core/Logger.cpp
    src/core/FuzzyMatcher.cpp
    src/core/FileScanner.cpp
    src/core/FileSearcher.cpp
    src/core/FileIndex.cpp
    src/core/SnapshotManager.cpp
    src/database/DatabaseMigrator.cpp
    src/database/FileIndexDatabase.cpp
    include/core/FileIndex.h
    include/core/FileSnapshot.h
    include/core/FileScanner.h
    include/core/SnapshotManager.h
    include/database/DatabaseMigrator.h
    include/database/FileIndexDatabase.h
)

add_executable(assistant_golden
    tests/golden_file_index.cpp
    ${ASSISTANT_INDEX_TEST_SOURCES}
)
add_executable(assistant_a3
    tests/a3_acceptance.cpp
    ${ASSISTANT_INDEX_TEST_SOURCES}
)

foreach(_acceptance_target assistant_golden assistant_a3)
    target_include_directories(${_acceptance_target} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    target_compile_definitions(${_acceptance_target} PRIVATE
        APP_VERSION="${PROJECT_VERSION}"
    )
    target_link_libraries(${_acceptance_target} PRIVATE
        Qt6::Core
        Qt6::Sql
        spdlog::spdlog
    )
endforeach()

