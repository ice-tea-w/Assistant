#!/usr/bin/env pwsh
#requires -Version 5.1
<#
.SYNOPSIS
    跑归档的 file_guard 单元测试（115 个）。

.DESCRIPTION
    归档把原工程的 `file_guard/` 包放在 tools/file_guard/ 下，包名仍是 `file_guard`，
    所以测试需要 PYTHONPATH 指向 tools/ 才能 `import file_guard`。本脚本负责这一步，
    然后按原工程的命令跑 unittest discover（start dir = tests/，helpers 由同目录导入）。

    依赖：Python 3.12+ 与 `cryptography`（`pip install -r tools/file_guard/requirements.txt`）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\file_guard\run-tests.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\file_guard\run-tests.ps1 -Python "C:\Python312\python.exe"
#>
[CmdletBinding()]
param(
    [string]$Python = 'python'
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$toolsDir = Split-Path -Parent $here

if (-not (Test-Path (Join-Path $here 'tests'))) {
    throw "找不到 $here\tests —— 归档不完整？"
}

$env:PYTHONPATH = $toolsDir
& $Python -m unittest discover -s (Join-Path $here 'tests') -t (Join-Path $here 'tests')
exit $LASTEXITCODE
