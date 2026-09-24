#pragma once

#include <QString>

/// Windows 开机启动（Phase 3 收尾）。
///
/// 实现方式：直接读写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`
/// 下的 `Assistant` 值——通过 `QSettings` 的 `NativeFormat`，不引额外依赖、
/// 不改 CMake 链接（Windows API 那套 RegOpenKeyExW 需要额外链 advapi32）。
///
/// 设计约定：**注册表本身是唯一真相来源**，不把该状态写进 `config.json`
/// （写两处就会出现"配置文件说开了、注册表里没有"的不一致）。
/// `ConfigManager::autoStart()` 只是每次启动时从注册表同步一份缓存给 QML。
namespace AutoStart
{
    /// 注册表值的名字（"Assistant"）。
    QString entryName();

    /// 期望写入的命令行：带引号的 exe 绝对路径（路径含空格时必须加引号）。
    QString command();

    /// 注册表里是否已存在指向**当前 exe 路径**的启动项。
    /// 值存在但指向别的路径（程序被移动过）时返回 false，便于设置页重新写入。
    bool isEnabled();

    /// 写入 / 删除启动项。返回是否成功（失败会记 warn 日志）。
    bool setEnabled(bool enabled);
}
