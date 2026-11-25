<#
.SYNOPSIS
  Assistant 一键构建（自动找 cmake / vcpkg，缺依赖时给中文修复步骤）

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release

.EXAMPLE
  # 显式指定 vcpkg / cmake
  scripts\build.ps1 -VcpkgRoot E:\vcpkg -CmakeExe "E:\...\cmake.exe"
#>
param(
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [switch]$ConfigureOnly,
    [string]$VcpkgRoot = '',
    [string]$CmakeExe = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Find-Cmake {
    if ($CmakeExe -and (Test-Path $CmakeExe)) { return $CmakeExe }
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $known = @(
        'E:\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    )
    foreach ($p in $known) { if (Test-Path $p) { return $p } }

    foreach ($vsRoot in @('C:\Program Files\Microsoft Visual Studio\2022',
                          'C:\Program Files (x86)\Microsoft Visual Studio\2022',
                          'E:\Microsoft Visual Studio\18')) {
        if (-not (Test-Path $vsRoot)) { continue }
        $hit = Get-ChildItem $vsRoot -Recurse -Depth 8 -Filter 'cmake.exe' -ErrorAction SilentlyContinue |
               Where-Object { $_.FullName -match '\\CMake\\bin\\cmake\.exe$' } |
               Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return ''
}

function Find-Vcpkg {
    param([string]$explicitRoot)
    $marker = 'scripts\buildsystems\vcpkg.cmake'
    if ($explicitRoot) {
        if (Test-Path (Join-Path $explicitRoot $marker)) { return $explicitRoot }
        throw "指定的 vcpkg 根目录无效：$explicitRoot（找不到 $marker）"
    }
    if ($env:VCPKG_ROOT -and (Test-Path (Join-Path $env:VCPKG_ROOT $marker))) {
        return $env:VCPKG_ROOT
    }
    foreach ($p in @('C:\vcpkg', 'D:\vcpkg', 'E:\vcpkg', 'D:\STUDY\vcpkg', 'E:\STUDY\vcpkg')) {
        if (Test-Path (Join-Path $p $marker)) { return $p }
    }
    return ''
}

$cmake = Find-Cmake
Write-Host "仓库根目录 : $Root"
if (-not $cmake) {
    Write-Host "[X] 未找到 cmake。" -ForegroundColor Red
    Write-Host "  可选：winget install Kitware.CMake（装完重开终端）"
    Write-Host "  或  : 用 VS Installer 给 VS 加装 'C++ CMake 工具' 组件"
    exit 2
}
Write-Host "cmake      : $cmake"

$vcpkg = Find-Vcpkg -explicitRoot $VcpkgRoot
if (-not $vcpkg) {
    Write-Host "[X] 未找到 vcpkg（本项目的 Qt6/spdlog/nlohmann_json/miniz 都从 vcpkg 来）。" -ForegroundColor Red
    Write-Host ""
    Write-Host "一次性安装步骤（约 30–60 分钟，Qt6 需要源码编译；磁盘预留 ~10 GB）：" -ForegroundColor Yellow
    Write-Host "  1) git clone https://github.com/microsoft/vcpkg E:\vcpkg"
    Write-Host "  2) E:\vcpkg\bootstrap-vcpkg.bat"
    Write-Host "  3) E:\vcpkg\vcpkg.exe install qtbase qtdeclarative qtquickcontrols2 spdlog nlohmann-json miniz sqlite3 --triplet x64-windows"
    Write-Host "  4) 重新运行：powershell -ExecutionPolicy Bypass -File scripts\build.ps1"
    Write-Host ""
    Write-Host "如果本机不方便装：请用组里已有 vcpkg 的机器构建 build\Debug，再把整个 build\Debug 目录拷回来（run.bat 里是相对路径）。"
    exit 3
}
Write-Host "vcpkg      : $vcpkg"

$toolchain = Join-Path $vcpkg 'scripts\buildsystems\vcpkg.cmake'
Write-Host "toolchain  : $toolchain"
Write-Host "config     : $Config"
Write-Host ""

& $cmake -B build -S . `
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] configure 失败（exit $LASTEXITCODE）。常见原因：vcpkg 依赖没装全，按上面 vcpkg install 补齐。" -ForegroundColor Red
    exit $LASTEXITCODE
}

if ($ConfigureOnly) {
    Write-Host "[OK] configure 完成（-ConfigureOnly，不编译）" -ForegroundColor Green
    exit 0
}

& $cmake --build build --config $Config
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] 编译失败（exit $LASTEXITCODE）" -ForegroundColor Red
    exit $LASTEXITCODE
}

$run = Join-Path $Root "build\$Config\run.bat"
Write-Host ""
if (Test-Path $run) {
    Write-Host "[OK] 构建完成：$run" -ForegroundColor Green
    Write-Host "     启动后主面板左侧 → 安全防护 → 运行安全体检"
} else {
    Write-Host "[OK] 构建完成（未找到 run.bat，检查 build\$Config）" -ForegroundColor Green
}
