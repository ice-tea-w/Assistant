#requires -Version 5.1
<#
.SYNOPSIS
  把 Assistant 的 Release 构建整理成一个可独立运行的目录（供 Inno Setup 打包）。

.DESCRIPTION
  - 程序文件与用户数据分离：本脚本只产出“程序目录”，不碰 %LOCALAPPDATA% 下的数据；
  - Qt 依赖全部相对化：复制 Qt6 DLL / plugins / qml，并生成 Prefix=. 的 qt.conf；
  - 依赖用 dumpbin 递归解析，避免“只拷 Qt6*.dll，漏 sqlite3.dll / 插件依赖”的老问题。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\deploy.ps1 -Config Release
  # → dist\Assistant-0.3.0\ （可直接运行 Assistant.exe）
#>
param(
    [string]$BuildDir = "build",
    [ValidateSet("Release", "Debug")][string]$Config = "Release",
    [string]$OutDir = "",
    [string]$QtRoot = "C:\vcpkg\installed\x64-windows",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

if (-not $Version) {
    $line = Select-String -Path (Join-Path $Root "CMakeLists.txt") `
                          -Pattern 'project\(\w+ VERSION ([0-9]+\.[0-9]+\.[0-9]+)' |
            Select-Object -First 1
    $Version = if ($line) { $line.Matches[0].Groups[1].Value } else { "0.0.0" }
}
if (-not $OutDir) { $OutDir = Join-Path $Root "dist\Assistant-$Version" }
$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$distRoot = [System.IO.Path]::GetFullPath((Join-Path $Root "dist"))
if (-not $OutDir.StartsWith($distRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "输出目录必须在 $distRoot 下：$OutDir"
}

$binDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    Join-Path $BuildDir $Config
} else {
    Join-Path $Root (Join-Path $BuildDir $Config)
}
if (-not (Test-Path (Join-Path $binDir "Assistant.exe"))) {
    throw "找不到 $binDir\Assistant.exe；先构建：cmake --build build --config $Config --target Assistant file_guard"
}
if (-not (Test-Path (Join-Path $binDir "file_guard.exe"))) {
    throw "找不到 $binDir\file_guard.exe（安全模块必须与主程序同目录）"
}

$qmlSrc = Join-Path $QtRoot "Qt6\qml"
$pluginSrc = Join-Path $QtRoot "Qt6\plugins"
$qtBin = Join-Path $QtRoot "bin"
foreach ($p in $qmlSrc, $pluginSrc, $qtBin) {
    if (-not (Test-Path $p)) { throw "Qt 目录不完整：$p" }
}

# dumpbin：从 vswhere 找最新 MSVC
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -property installationPath
    if ($vsPath) {
        $dumpbin = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
                   ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\dumpbin.exe" } |
                   Where-Object { Test-Path $_ } |
                   Sort-Object -Descending | Select-Object -First 1
    }
}
if (-not $dumpbin) { throw "找不到 dumpbin.exe（需要 VS 2022 的 MSVC 工具链）" }

Write-Host "==> 清理 $OutDir"
if (Test-Path $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
New-Item -ItemType Directory -Path $OutDir | Out-Null

Write-Host "==> 复制主程序"
foreach ($exe in "Assistant.exe", "file_guard.exe") {
    Copy-Item (Join-Path $binDir $exe) (Join-Path $OutDir $exe) -Force
}

Write-Host "==> 生成相对化 qt.conf"
@"
[Paths]
Prefix = .
Plugins = Qt6/plugins
QmlImports = Qt6/qml
"@ | Set-Content -Path (Join-Path $OutDir "qt.conf") -Encoding ascii

Write-Host "==> 复制 Qt QML（只带用到的模块）"
$qmlDst = Join-Path $OutDir "Qt6\qml"
New-Item -ItemType Directory -Path $qmlDst -Force | Out-Null
# QtQuick 根目录的 qmldir / 顶层 qml
Get-ChildItem (Join-Path $qmlSrc "QtQuick") -File | Copy-Item -Destination $qmlDst -Force
foreach ($module in "QtQuick\Controls", "QtQuick\Dialogs", "QtQuick\Effects", "QtQuick\Layouts",
                   "QtQuick\Shapes", "QtQuick\Templates", "QtQuick\Window", "QtQuick\VectorImage",
                   "QtQml", "QML") {
    $src = Join-Path $qmlSrc $module
    if (-not (Test-Path $src)) { Write-Warning "跳过不存在的 QML 模块：$module"; continue }
    $dst = Join-Path $qmlDst $module
    New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
    Copy-Item $src $dst -Recurse -Force
}
# QtQuick.Dialogs 的 quickimpl 依赖 Qt5Compat.GraphicalEffects 时才需要；带了也不大
$compat = Join-Path $qmlSrc "Qt5Compat\GraphicalEffects"
if (Test-Path $compat) {
    $dst = Join-Path $qmlDst "Qt5Compat\GraphicalEffects"
    New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
    Copy-Item $compat $dst -Recurse -Force
}
$labs = Join-Path $qmlSrc "Qt\labs\folderlistmodel"
if (Test-Path $labs) {
    $dst = Join-Path $qmlDst "Qt\labs\folderlistmodel"
    New-Item -ItemType Directory -Path (Split-Path $dst -Parent) -Force | Out-Null
    Copy-Item $labs $dst -Recurse -Force
}

Write-Host "==> 复制 Qt 插件"
$pluginDst = Join-Path $OutDir "Qt6\plugins"
foreach ($dir in "platforms", "styles", "sqldrivers", "imageformats", "iconengines",
                 "tls", "networkinformation", "platforminputcontexts") {
    $src = Join-Path $pluginSrc $dir
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $pluginDst $dir
    New-Item -ItemType Directory -Path $dst -Force | Out-Null
    Get-ChildItem $src -File | Copy-Item -Destination $dst -Force
}

# 显式补 qsqlite 的底层依赖（qsqlite.dll 链接 sqlite3.dll，vcpkg app-local 不会带）
$sqlite = Join-Path $qtBin "sqlite3.dll"
if (Test-Path $sqlite) { Copy-Item $sqlite (Join-Path $OutDir "sqlite3.dll") -Force }

Write-Host "==> 递归解析并复制 DLL 依赖"
$queue = New-Object System.Collections.Queue
$queued = @{}
function Add-ToQueue([string]$path) {
    $full = [System.IO.Path]::GetFullPath($path)
    if (-not $queued.ContainsKey($full) -and (Test-Path $full)) {
        $queued[$full] = $true
        $queue.Enqueue($full)
    }
}

Get-ChildItem $OutDir -File -Filter *.exe | ForEach-Object { Add-ToQueue $_.FullName }
Get-ChildItem (Join-Path $OutDir "Qt6") -Recurse -File -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Add-ToQueue $_.FullName }

$vcRuntime = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
while ($queue.Count -gt 0) {
    $file = $queue.Dequeue()
    $deps = & $dumpbin /nologo /dependents $file 2>$null
    foreach ($line in $deps) {
        if ($line -match '^\s+([A-Za-z0-9_.\-]+\.dll)\s*$') {
            $name = $Matches[1]
            $candidate = Join-Path $qtBin $name
            if (Test-Path $candidate) {
                $target = Join-Path $OutDir $name
                if (-not (Test-Path $target)) {
                    Copy-Item $candidate $target -Force
                }
                Add-ToQueue $target
            } elseif ($vcRuntime -contains $name.ToLower()) {
                $sys = Join-Path $env:WINDIR "System32\$name"
                if (Test-Path $sys) {
                    $target = Join-Path $OutDir $name
                    if (-not (Test-Path $target)) { Copy-Item $sys $target -Force }
                    Add-ToQueue $target
                }
            }
        }
    }
}

# 兜底：常见第三方依赖（万一 dumpbin 解析漏了）
foreach ($name in "sqlite3.dll", "miniz.dll", "spdlog.dll", "fmt.dll") {
    $src = Join-Path $qtBin $name
    if ((Test-Path $src) -and -not (Test-Path (Join-Path $OutDir $name))) {
        Copy-Item $src (Join-Path $OutDir $name) -Force
    }
}

# 发布包不带 PDB（Qt 插件/qml 里的 .pdb 体积很大，运行时用不到）
Get-ChildItem $OutDir -Recurse -File -Filter *.pdb -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

$files = Get-ChildItem $OutDir -Recurse -File
$sizeMb = ($files | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("==> 完成：{0}" -f $OutDir)
Write-Host ("    {0} 个文件，{1:N1} MB" -f $files.Count, $sizeMb)
Write-Host ("    版本：{0}" -f $Version)
