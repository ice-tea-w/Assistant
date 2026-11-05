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
    Core Quick QuickControls2 Network Widgets Sql Test
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

# ---- 资源依赖（重要）----
# AUTORCC 的依赖扫描并不可靠地覆盖 .qrc 里"仅被列出"的文件：
# 只改 .qml 内容时可能不触发重新打包，于是"改了界面但跑起来还是旧的"。
# 显式把它们声明成 resources.qrc 的依赖，改任何一个 QML 都会重新 rcc。
file(GLOB_RECURSE QRC_RESOURCE_FILES
    resources/qml/*.qml
    resources/qml/qmldir
    resources/market/*.json
)
if (QRC_RESOURCE_FILES)
    set_property(SOURCE resources/resources.qrc PROPERTY
        OBJECT_DEPENDS "${QRC_RESOURCE_FILES}")
endif()

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
    target_link_libraries(${PROJECT_NAME} PRIVATE shell32 ole32 crypt32 advapi32)
endif()

# ---- Qt 部署 ----
# 开发阶段：在 exe 旁生成 qt.conf，指向 vcpkg 的 Qt6 安装目录。
# qt.conf 负责：platforms/、qml/ 的路径（Qt 内部查找）
# 但同时需要 vcpkg/bin 在 PATH 中（Windows DLL 加载器查找）

# Qt 安装前缀由 vcpkg toolchain 提供，不硬编码路径（换机器或换 vcpkg 位置都无需改这里）。
# 未定义只可能是没用 vcpkg toolchain 配置，此时静默生成错误路径比直接报错难查得多。
if(NOT VCPKG_INSTALLED_DIR)
    message(FATAL_ERROR "VCPKG_INSTALLED_DIR 未定义：请按 CLAUDE.md 的命令带 CMAKE_TOOLCHAIN_FILE 配置")
endif()

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

# ---- 测试 / 验收 ----
# 三个工具共用同一份最小源集合：A2 的 golden 对比、A3 的采集/持久化验收、
# A4 的 baseline。只链 Core + Sql：它们都不碰 GUI/QML，也不需要 ConfigManager。
set(ASSISTANT_INDEX_TEST_SOURCES
    src/core/Logger.cpp
    src/core/FuzzyMatcher.cpp
    src/core/ScoreEngine.cpp
    src/core/QueryParser.cpp
    src/core/QueryEngine.cpp
    src/core/FileScanner.cpp
    src/core/FileSearcher.cpp
    src/core/FileIndex.cpp
    src/core/FileEventDispatcher.cpp
    src/core/SnapshotManager.cpp
    src/database/DatabaseMigrator.cpp
    src/database/FileIndexDatabase.cpp
    include/core/FileIndex.h
    include/core/FileSnapshot.h
    include/core/FileScanner.h
    include/core/QueryEngine.h
    include/core/QueryParser.h
    include/core/ScoreEngine.h
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
add_executable(assistant_bench
    tests/bench_file_index.cpp
    ${ASSISTANT_INDEX_TEST_SOURCES}
)

foreach(_acceptance_target assistant_golden assistant_a3 assistant_bench)
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

# A4：打开测试登记（架构文档 §A4 的 CMake 要求）。
# 只登记"快、稳、无外部依赖"的用例；bench 是测量工具，不作为 test 跑。
enable_testing()
add_test(NAME golden_file_index COMMAND assistant_golden --n=20000)
add_test(NAME a3_migrate         COMMAND assistant_a3 --migrate)
add_test(NAME a3_prune           COMMAND assistant_a3 --prune)

# a3_migrate 要用 Qt 的 QSQLITE 驱动：qsqlite.dll 自己链接 sqlite3.dll，而
# **vcpkg 的 app-local 拷贝不含 sqlite3.dll**（构建目录里只自动拷了 Qt6*.dll）。
# Debug 下一直被 run.bat / 调试命令里的 vcpkg PATH 掩盖着，ctest 才暴露出来。
# 打包阶段必须补上这个依赖 —— 见 `docs/devlog/2026-10-22.md` 的"部署缺口"。
#
# 路径由 toolchain 提供，**不硬编码 C:/vcpkg**：本机 vcpkg 在 D:/STUDY/vcpkg，
# 写死会让 ctest 在别的机器/别的 vcpkg 位置上假失败（合并 plugin-market 时两边
# 各自修过这一点，这里是两者的并集：main 的变量式写法 + Debug 目录）。
set(VCPKG_BIN_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
set(VCPKG_DBG_BIN_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")

set_tests_properties(a3_migrate PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- 测试（QTest；用 vcpkg 已装的 Qt6Test，不引入 GTest） ----
# 只编被测的纯逻辑与存储层，不编 UI/main：
# SyncEngine/SyncBridge 依赖线程与 QML，留给集成测试（单机双实例手工走查）。

add_executable(assistant_tests
    tests/test_sync.cpp
    # Q_OBJECT 头必须列进目标源，否则 AUTOMOC 不会为它生成 moc
    include/core/sync/SyncEngine.h
    include/core/ConfigManager.h
    src/core/Logger.cpp
    src/core/sync/ConflictResolver.cpp
    src/core/sync/PathGuard.cpp
    src/core/sync/FolderTransport.cpp
    src/core/sync/SyncEngine.cpp
    src/core/sync/DeliveryNaming.cpp
    src/core/sync/SyncCommands.cpp
    src/database/SyncDatabase.cpp
    # 配置层往返也要测（同步根列表靠它持久化）
    src/core/ConfigManager.cpp
    src/core/AutoStart.cpp
)

target_include_directories(assistant_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(assistant_tests PRIVATE
    Qt6::Core
    Qt6::Sql
    # 直连通道前提自检要 QSslSocket（TLS 后端是否可用）
    Qt6::Network
    Qt6::Test
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

add_test(NAME sync_unit COMMAND assistant_tests)
# 同步单测会加载 QML 插件（qmlplugin.dll），需要 Qt 的 bin 在 PATH 里
set_tests_properties(sync_unit PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- A5：FileWatcher / FileEventDispatcher 单测 ----
# 不编 FileIndex：这里验证的是"事件源"本身（真实目录 + ReadDirectoryChangesW）。
add_executable(assistant_watch_tests
    tests/test_file_watcher.cpp
    include/core/FileWatcher.h
    src/core/FileWatcher.cpp
    src/core/FileEventDispatcher.cpp
    src/core/Logger.cpp
)

target_include_directories(assistant_watch_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(assistant_watch_tests PRIVATE
    Qt6::Core
    Qt6::Test
    spdlog::spdlog
)

add_test(NAME watch_unit COMMAND assistant_watch_tests)
set_tests_properties(watch_unit PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- A5b：事件 → 索引 → 查询 的端到端 ----
add_executable(assistant_index_tests
    tests/test_file_index_incremental.cpp
    ${ASSISTANT_INDEX_TEST_SOURCES}
    include/core/FileIndex.h
    include/core/FileWatcher.h
    src/core/FileWatcher.cpp
    src/core/FileEventDispatcher.cpp
)

target_include_directories(assistant_index_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_compile_definitions(assistant_index_tests PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(assistant_index_tests PRIVATE
    Qt6::Core
    Qt6::Sql
    Qt6::Test
    spdlog::spdlog
)

add_test(NAME index_incremental COMMAND assistant_index_tests)
set_tests_properties(index_incremental PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- A7：provider + k-way merge ----
add_executable(assistant_search_tests
    tests/test_search_providers.cpp
    src/ui/AppBridge.cpp
    src/core/Calculator.cpp
    src/database/HistoryDatabase.cpp
    include/ui/AppBridge.h
    include/database/HistoryDatabase.h
    ${ASSISTANT_INDEX_TEST_SOURCES}
)
target_include_directories(assistant_search_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(assistant_search_tests PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(assistant_search_tests PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Test
    spdlog::spdlog nlohmann_json::nlohmann_json)

add_test(NAME search_providers COMMAND assistant_search_tests)
set_tests_properties(search_providers PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- N0：公网直连的帧编解码与地址解析（纯函数，零 IO）----
add_executable(assistant_net_tests
    tests/test_net_protocol.cpp
    src/core/net/SyncFrame.cpp
    src/core/net/PeerAddress.cpp
)
target_include_directories(assistant_net_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(assistant_net_tests PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_test(NAME net_protocol COMMAND assistant_net_tests)
set_tests_properties(net_protocol PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- N1：设备身份（自签 RSA-2048，CryptoAPI）----
add_executable(assistant_net_identity_tests
    tests/test_net_identity.cpp
    src/core/net/Identity.cpp
)
target_include_directories(assistant_net_identity_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(assistant_net_identity_tests PRIVATE
    Qt6::Core Qt6::Network Qt6::Test
    bcrypt crypt32)

add_test(NAME net_identity COMMAND assistant_net_identity_tests)
set_tests_properties(net_identity PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")

# ---- N1：回环 TLS（服务端 + 双向 mTLS，验 Schannel 能否用内存私钥）----
add_executable(assistant_net_tls_tests
    tests/test_net_tls.cpp
    src/core/net/Identity.cpp
    src/core/net/SyncFrame.cpp
    src/core/net/DeviceAuth.cpp
)
target_include_directories(assistant_net_tls_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(assistant_net_tls_tests PRIVATE
    Qt6::Core Qt6::Network Qt6::Test
    bcrypt crypt32)

add_test(NAME net_tls COMMAND assistant_net_tls_tests)
set_tests_properties(net_tls PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")
# ---- N1：客户端认证（单向 TLS 之后的挑战应答，Schannel 不支持双向 mTLS）----
add_executable(assistant_net_auth_tests
    tests/test_net_auth.cpp
    src/core/net/DeviceAuth.cpp
)
target_include_directories(assistant_net_auth_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(assistant_net_auth_tests PRIVATE Qt6::Core Qt6::Test)

add_test(NAME net_auth COMMAND assistant_net_auth_tests)
set_tests_properties(net_auth PROPERTIES
    ENVIRONMENT "PATH=${VCPKG_BIN_DIR};${VCPKG_DBG_BIN_DIR};$ENV{PATH}")