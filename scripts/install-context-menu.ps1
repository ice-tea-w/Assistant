# Windows 资源管理器右键菜单注册（仅写 HKCU，不需要管理员权限）
#
# 作用：给文件/文件夹右键菜单加两项：
#   1) 用 FileGuard 加密
#   2) 用 FileGuard 解密（--force，覆盖同名明文）
#
# 用法（在 PowerShell 里手动执行，脚本不会自动被调用）：
#   powershell -ExecutionPolicy Bypass -File scripts\install-context-menu.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\install-context-menu.ps1 -Uninstall
#
# 说明：这是"右键菜单"的最小可用形态：点击后弹出控制台窗口，由 CLI 交互式输入密码。
# 正式产品建议换成 pythonw + tkinter 小对话框，或直接在桌面助手的 UI 里做按钮，
# 避免用户看到控制台窗口；同时密码输入框不要回显。

param(
    [string]$PythonExe = "",
    [string]$PackageDir = "",
    [string]$Home = "",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

if (-not $PythonExe) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "找不到 python，请用 -PythonExe 指定解释器路径" }
    $PythonExe = $cmd.Source
}
if (-not $PackageDir) {
    $PackageDir = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
}
$PackageDir = (Resolve-Path $PackageDir).Path
if (-not (Test-Path (Join-Path $PackageDir "file_guard\cli.py"))) {
    throw "PackageDir 不正确（应包含 file_guard\cli.py）：$PackageDir"
}
if (-not $Home) {
    $Home = Join-Path $env:USERPROFILE ".file_guard"
}

$appDir = Join-Path $env:LOCALAPPDATA "FileGuard"
$cmdFile = Join-Path $appDir "file_guard.cmd"
$classesRoot = "HKCU:\Software\Classes\*\shell"

function Remove-Menu {
    foreach ($verb in @("FileGuardEncrypt", "FileGuardDecrypt")) {
        $key = Join-Path $classesRoot $verb
        if (Test-Path $key) {
            Remove-Item $key -Recurse -Force
            Write-Host "已移除右键菜单：$verb"
        }
    }
    if (Test-Path $cmdFile) { Remove-Item $cmdFile -Force }
    Write-Host "卸载完成（用户数据目录 $Home 未删除）"
}

if ($Uninstall) {
    Remove-Menu
    exit 0
}

# 1) 生成包装脚本：解决 `python -m file_guard` 在任意工作目录下的导入问题
New-Item -ItemType Directory -Path $appDir -Force | Out-Null
@"
@echo off
set "PYTHONPATH=$PackageDir;%PYTHONPATH%"
"$PythonExe" -m file_guard %*
"@ | Set-Content -Path $cmdFile -Encoding ASCII

# 2) 注册两个动作
$verbs = @{
    "FileGuardEncrypt" = @{
        Title = "用 FileGuard 加密"
        Args  = "encrypt `"%1`" --home `"$Home`""
    }
    "FileGuardDecrypt" = @{
        Title = "用 FileGuard 解密"
        Args  = "decrypt `"%1`" --home `"$Home`" --force"
    }
}

foreach ($name in $verbs.Keys) {
    $key = Join-Path $classesRoot $name
    New-Item -Path $key -Force | Out-Null
    Set-ItemProperty -Path $key -Name "(Default)" -Value $verbs[$name].Title
    Set-ItemProperty -Path $key -Name "Icon" -Value "$PythonExe,0"
    # 支持一次选中多个文件
    Set-ItemProperty -Path $key -Name "MultiSelectModel" -Value "Player"
    $commandKey = Join-Path $key "command"
    New-Item -Path $commandKey -Force | Out-Null
    Set-ItemProperty -Path $commandKey -Name "(Default)" -Value "`"$cmdFile`" $($verbs[$name].Args)"
    Write-Host "已注册右键菜单：$($verbs[$name].Title)"
}

Write-Host ""
Write-Host "完成。包装脚本：$cmdFile"
Write-Host "数据目录：$Home"
Write-Host "提示：右键菜单对 .fgc 容器和普通文件都会出现，按需点击即可。"
