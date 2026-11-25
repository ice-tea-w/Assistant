<#
.SYNOPSIS
  Assistant 安全模块（M3）· 一键验收演示（Windows PowerShell 5.1+）

.DESCRIPTION
  在隔离临时目录里跑完整闭环：加密/校验/解密/扫描/审计/备份/保险箱/自检 + 两类攻击演示。
  不碰真实 ~/.file_guard；只清理本次目录（-Keep 保留现场）。不要并发跑多个实例。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\acceptance-demo.ps1

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\acceptance-demo.ps1 -Full
  # 额外跑 160 条单元测试 + 52 项端到端验收（约 30 秒）
#>
param(
    [switch]$Full,
    [switch]$Keep,
    [string]$WorkDir = ""   # 可选：把临时现场放到可写目录（受限环境用）
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$py = $env:ASSISTANT_PYTHON
if (-not $py) { $c = Join-Path $Root '.venv\Scripts\python.exe'; if (Test-Path $c) { $py = $c } }
if (-not $py) { $c = Join-Path $Root 'tools\file_guard\.venv\Scripts\python.exe'; if (Test-Path $c) { $py = $c } }
if (-not $py) { $cmd = Get-Command python -ErrorAction SilentlyContinue; if ($cmd) { $py = $cmd.Source } }
if (-not $py) { throw '未找到 Python（可设 ASSISTANT_PYTHON 或建 .venv）' }
$env:PYTHONPATH = (Join-Path $Root 'tools') + ';' + $env:PYTHONPATH
if (-not (Test-Path (Join-Path $Root '合同.docx'))) { throw "未找到演示文件 合同.docx（应在项目根目录）" }

if ($WorkDir -ne "") { $work = $WorkDir } else { $work = Join-Path $Root ("_accept-" + [guid]::NewGuid().ToString('N').Substring(0, 8)) }
New-Item -ItemType Directory -Path $work -Force | Out-Null
$homeDir = Join-Path $work 'home'
$workDir = Join-Path $work 'work'
New-Item -ItemType Directory -Path $workDir -Force | Out-Null
$src = Join-Path $workDir '合同.docx'
Copy-Item (Join-Path $Root '合同.docx') $src -Force
$container = Join-Path $workDir '合同.docx.fgc'

$env:FG_PASSWORD = 'accept-demo-pass-123'
$env:FG_KDF_ITERATIONS = '1000'
$env:PYTHONIOENCODING = 'utf-8'

$script:results = @()

function Invoke-FG {
    # 输出直接打到控制台，返回退出码（避免被赋值时把 stdout 也吞进去）
    & $py -m file_guard @args | Out-Host
    return $LASTEXITCODE
}

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Test-Step {
    param([string]$Name, [scriptblock]$Block)
    Write-Host "`n=== $Name ===" -ForegroundColor Cyan
    try {
        & $Block
        $script:results += [pscustomobject]@{ 序号 = $script:results.Count + 1; 检查项 = $Name; 结果 = 'PASS'; 说明 = '' }
        Write-Host "[PASS] $Name" -ForegroundColor Green
    } catch {
        $script:results += [pscustomobject]@{ 序号 = $script:results.Count + 1; 检查项 = $Name; 结果 = 'FAIL'; 说明 = $_.Exception.Message }
        Write-Host "[FAIL] $Name : $($_.Exception.Message)" -ForegroundColor Red
    }
}

Write-Host "Assistant 安全模块（M3）· 一键验收演示" -ForegroundColor Yellow
Write-Host "隔离目录：$work"

try {
    Test-Step '01 初始化数据目录' {
        $code = Invoke-FG init --home $homeDir --no-vault
        Assert-True ($code -eq 0) "init exit=$code"
        Assert-True (Test-Path (Join-Path $homeDir 'config.json')) 'config.json 未生成'
    }

    Test-Step '02 加密真实文件 → 生成 .fgc 容器' {
        $code = Invoke-FG encrypt $src --home $homeDir
        Assert-True ($code -eq 0) "encrypt exit=$code"
        Assert-True (Test-Path $container) '容器未生成'
        $magic = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($container), 0, 4)
        Assert-True ($magic -eq 'FGC1') "容器 magic 不是 FGC1：$magic"
    }

    Test-Step '03 verify：容器明文哈希与源文件一致' {
        $code = Invoke-FG verify $container --home $homeDir
        Assert-True ($code -eq 0) "verify exit=$code"
    }

    Test-Step '04 解密并逐字节比对' {
        $out = Join-Path $work 'restored.docx'
        $code = Invoke-FG decrypt $container -o $out --home $homeDir --force
        Assert-True ($code -eq 0) "decrypt exit=$code"
        Assert-True ((Get-FileHash $src).Hash -eq (Get-FileHash $out).Hash) '明文哈希不一致'
    }

    Test-Step '05 敏感扫描：命中中危项且样本打码' {
        $json = & $py -m file_guard scan '合同.docx' --home $homeDir --json | ConvertFrom-Json
        Assert-True ($LASTEXITCODE -eq 0) "scan exit=$LASTEXITCODE"
        Assert-True ($json.summary.medium -ge 1) '未命中中危项'
        Assert-True ($json.files[0].findings.Count -ge 1) '没有发现项'
    }

    Test-Step '06 审计链校验通过' {
        $code = Invoke-FG log verify --home $homeDir
        Assert-True ($code -eq 0) "log verify exit=$code"
    }

    Test-Step '07 备份 → 按版本回滚还原' {
        $code = Invoke-FG backup add $src --home $homeDir
        Assert-True ($code -eq 0) "backup add exit=$code"
        $dest = Join-Path $work 'v1.docx'
        $code = Invoke-FG backup restore $src --version 1 --dest $dest --home $homeDir
        Assert-True ($code -eq 0) "restore exit=$code"
        Assert-True ((Get-FileHash $dest).Hash -eq (Get-FileHash $src).Hash) '还原内容不一致'
    }

    Test-Step '08 保险箱：初始化/放入/随机容器名/取出/校验' {
        $info = & $py -m file_guard vault init --home $homeDir --no-dpapi --json | ConvertFrom-Json
        Assert-True ($LASTEXITCODE -eq 0) "vault init exit=$LASTEXITCODE"
        Assert-True ([bool]$info.recovery_code) '未生成恢复码'

        $code = Invoke-FG vault add $src --home $homeDir
        Assert-True ($code -eq 0) "vault add exit=$code"

        $vaultFiles = @(Get-ChildItem (Join-Path $homeDir 'vault\containers') -File)
        Assert-True ($vaultFiles.Count -eq 1) "保险箱容器数不对：$($vaultFiles.Count)"
        Assert-True ($vaultFiles[0].Name -match '^[0-9a-f]{32}\.fgc$') "容器名不是随机名：$($vaultFiles[0].Name)"

        $outDir = Join-Path $work 'vault-out'
        $code = Invoke-FG vault extract 1 -o $outDir --home $homeDir
        Assert-True ($code -eq 0) "vault extract exit=$code"
        Assert-True ((Get-FileHash (Join-Path $outDir '合同.docx')).Hash -eq (Get-FileHash $src).Hash) '保险箱取出内容不一致'

        $code = Invoke-FG vault verify --home $homeDir
        Assert-True ($code -eq 0) "vault verify exit=$code"
    }

    Test-Step '09 doctor 自检' {
        $code = Invoke-FG doctor --home $homeDir
        Assert-True ($code -eq 0) "doctor exit=$code"
    }

    Test-Step '10 攻击演示：改容器 1 字节 → 解密被拒(exit 3)且无明文残留' {
        $tampered = Join-Path $work 'tampered.fgc'
        Copy-Item $container $tampered -Force
        $bytes = [IO.File]::ReadAllBytes($tampered)
        $bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 0xFF
        [IO.File]::WriteAllBytes($tampered, $bytes)
        $evil = Join-Path $work 'evil.docx'
        $code = Invoke-FG decrypt $tampered -o $evil --home $homeDir
        Assert-True ($code -eq 3) "期望 exit 3，实际 $code"
        Assert-True (-not (Test-Path $evil)) '失败解密不应留下明文'
    }

    Test-Step '11 攻击演示：改审计一行 → log verify 失败(exit 5)' {
        $log = Join-Path $homeDir 'audit\audit.jsonl'
        $lines = [IO.File]::ReadAllLines($log)
        $row = $lines[0] | ConvertFrom-Json
        $row.h = ('00' * 32)
        $lines[0] = ($row | ConvertTo-Json -Compress)
        [IO.File]::WriteAllLines($log, $lines)
        $code = Invoke-FG log verify --home $homeDir
        Assert-True ($code -eq 5) "期望 exit 5，实际 $code"
    }

    if ($Full) {
        Test-Step '12 全量单元测试（160 条）' {
            & $py -m unittest discover -s tests -q | Out-Host
            Assert-True ($LASTEXITCODE -eq 0) "unittest exit=$LASTEXITCODE"
        }
        Test-Step '13 端到端验收脚本（52 项）' {
            & $py scripts\run_acceptance.py | Out-Host
            Assert-True ($LASTEXITCODE -eq 0) "run_acceptance exit=$LASTEXITCODE"
        }
    }
}
finally {
    if (-not $Keep) {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    } else {
        Write-Host "现场已保留：$work" -ForegroundColor Yellow
    }
}

Write-Host "`n================ 验收结果 ================" -ForegroundColor Yellow
$script:results | Format-Table -AutoSize | Out-String | Write-Host
$failed = @($script:results | Where-Object { $_.结果 -ne 'PASS' })
if ($failed.Count -eq 0) {
    Write-Host "全部通过（$($script:results.Count) 项）✔" -ForegroundColor Green
    exit 0
} else {
    Write-Host "有 $($failed.Count) 项失败：" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host ("  - {0}: {1}" -f $_.检查项, $_.说明) -ForegroundColor Red }
    exit 1
}
