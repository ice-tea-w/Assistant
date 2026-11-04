<#
.SYNOPSIS
    起一个本地"插件市场"，把整个对外链路一次演示出来（不联网）。

.DESCRIPTION
    做的事：
      1. 用 tools/plugin-pack.ps1 把 examples/text-tools 打成 1.0.0 与 1.1.0 两个包；
      2. 生成市场索引 index.json（含 url / sha256 / size），按 -Phase 决定 latest 是哪个版本；
      3. 用 TcpListener 起一个只读静态 HTTP 服务（127.0.0.1，不需要管理员权限）；
      4. 打印给 Assistamt 用的环境变量，可选直接拉起程序。

    两个阶段正好演示两个能力：
      -Phase 1  市场里是 1.0.0  → 演示"从市场安装"
      -Phase 2  市场里是 1.1.0  → 已装 1.0.0 的机器上出现「更新」角标

.PARAMETER Launch
    起完服务后直接启动 build\Release\run.bat（需要先编译过）。

.EXAMPLE
    .\tools\demo-market.ps1
    # 另开一个终端按它打印的方式启动程序

.EXAMPLE
    .\tools\demo-market.ps1 -Phase 2 -Launch
    # 装好 1.0.0 之后重跑这一条，重启程序就能看到「更新」
#>
[CmdletBinding()]
param(
    [int] $Port = 18730,
    [ValidateSet(1, 2)] [int] $Phase = 1,
    [switch] $Launch,
    [switch] $HealthCheck,
    [string] $RepoRoot = ""
)

$ErrorActionPreference = 'Stop'

# PS 5.1 里 param 默认值拿不到 $PSScriptRoot（求值时机太早），放正文里算。
if ([string]::IsNullOrEmpty($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}

$dist = Join-Path $RepoRoot 'dist\demo-market'
New-Item -ItemType Directory -Path $dist -Force | Out-Null

$pack = Join-Path $PSScriptRoot 'plugin-pack.ps1'
$example = Join-Path $RepoRoot 'examples\text-tools'

Write-Host "[1/3] 打包示例插件 1.0.0 …" -ForegroundColor Cyan
# 6>$null 3>$null：把 plugin-pack 的 Write-Host(信息流) 与 Write-Warning 吞掉，
# 演示时屏幕上只留本脚本自己的三步提示。
& $pack -PluginDir $example -OutDir $dist -NoEntryFile -WarningAction SilentlyContinue 6>$null 3>$null | Out-Null

Write-Host "[2/3] 造 1.1.0（同一份代码，只改版本号，用来演示更新）…" -ForegroundColor Cyan
$stage = Join-Path $env:TEMP ('demo-pkg-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -Path (Join-Path $example '*') -Destination $stage -Recurse -Force
$manifestPath = Join-Path $stage 'plugin.json'
$m = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$m.version = '1.1.0'
$m.description = '大小写转换 / 去重 / 统计（1.1.0 · 演示更新）'
[System.IO.File]::WriteAllText($manifestPath, (($m | ConvertTo-Json -Depth 10) + "`n"),
                               [System.Text.UTF8Encoding]::new($false))
& $pack -PluginDir $stage -OutDir $dist -NoEntryFile -WarningAction SilentlyContinue 6>$null 3>$null | Out-Null
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue

# ---- 索引 ----
$baseUrl = "http://127.0.0.1:$Port"
$latest  = if ($Phase -eq 1) { '1.0.0' } else { '1.1.0' }

function ReleaseOf([string] $version) {
    $zip  = Join-Path $dist "com.assistant.texttools-$version.zip"
    if (-not (Test-Path $zip)) { throw "缺包：$zip" }
    return [ordered]@{
        version = $version
        url     = "$baseUrl/com.assistant.texttools-$version.zip"
        sha256  = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
        size    = (Get-Item $zip).Length
        minAppVersion = '0.3.0'
        changelog = if ($version -eq '1.1.0') { '演示：市场出现新版本，卡片显示「更新」' } else { '首个版本' }
    }
}

$index = [ordered]@{
    schemaVersion = 1
    generatedAt   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    plugins = @(
        [ordered]@{
            id          = 'com.assistant.texttools'
            name        = '文本工具'
            summary     = '大小写转换 / 去重 / 统计（本地市场演示）'
            description = '这是 demo-market.ps1 现打的包，用来演示"接口如何对外"：没有改任何宿主代码。'
            author      = 'AssistantTeam'
            categories  = @('工具', '文本')
            tags        = @('演示')
            icon        = '🔤'
            latest      = (ReleaseOf $latest)
            versions    = @((ReleaseOf '1.0.0'), (ReleaseOf '1.1.0'))
            updatedAt   = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        }
    )
}

$indexPath = Join-Path $dist 'index.json'
[System.IO.File]::WriteAllText($indexPath, (($index | ConvertTo-Json -Depth 10) + "`n"),
                               [System.Text.UTF8Encoding]::new($false))
Write-Host "[3/3] 索引已生成：$indexPath" -ForegroundColor Cyan

# ---- 静态服务：TcpListener 而不是 HttpListener ----
# HttpListener 的 http://127.0.0.1:port 前缀在 Windows 上要预先注册 URL ACL（要管理员），
# TcpListener 不需要。这里只需要"能 GET 文件"。
$serverCode = {
    param($root, $port)

    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
    $listener.Start()

    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $reader = [System.IO.StreamReader]::new($stream)

            $requestLine = $reader.ReadLine()
            while ($true) {
                $header = $reader.ReadLine()
                if ([string]::IsNullOrEmpty($header)) { break }
            }

            $path = '/'
            if ($requestLine) {
                $parts = $requestLine.Split(' ')
                if ($parts.Length -ge 2) { $path = $parts[1] }
            }
            $path = $path.Split('?')[0].TrimStart('/')

            $file = Join-Path $root $path
            if ([string]::IsNullOrEmpty($path) -or -not (Test-Path $file -PathType Leaf)) {
                $body = [System.Text.Encoding]::UTF8.GetBytes('not found')
                $head = "HTTP/1.1 404 Not Found`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n"
                $bytes = [System.Text.Encoding]::ASCII.GetBytes($head)
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Write($body, 0, $body.Length)
            }
            else {
                $body  = [System.IO.File]::ReadAllBytes($file)
                $mime  = if ($file.EndsWith('.json')) { 'application/json' } else { 'application/zip' }
                $head  = "HTTP/1.1 200 OK`r`nContent-Type: $mime`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n"
                $bytes = [System.Text.Encoding]::ASCII.GetBytes($head)
                $stream.Write($bytes, 0, $bytes.Length)
                $stream.Write($body, 0, $body.Length)
            }
            $stream.Flush()
        }
        catch {
            # 单个连接出错不该带走整个服务
        }
        finally {
            $client.Close()
        }
    }
}

$job = Start-Job -ScriptBlock $serverCode -ArgumentList $dist, $Port
Start-Sleep -Milliseconds 900

if ($job.State -eq 'Failed') {
    Write-Host "服务启动失败：" -ForegroundColor Red
    Receive-Job $job | Write-Host
    Remove-Job $job -Force
    exit 1
}

try {
    $probe = Invoke-WebRequest "$baseUrl/index.json" -UseBasicParsing -TimeoutSec 5
    if ($probe.StatusCode -ne 200) { throw "索引返回 $($probe.StatusCode)" }
}
catch {
    Write-Host "自检失败（端口 $Port 可能被占用）：$($_.Exception.Message)" -ForegroundColor Red
    Stop-Job $job; Remove-Job $job -Force
    exit 1
}

# 自检模式：证明"包 + 索引 + 服务"这一套是通的，然后退出，不进入前台等待。
if ($HealthCheck) {
    $zipName = "com.assistant.texttools-$latest.zip"
    $zipProbe = Invoke-WebRequest "$baseUrl/$zipName" -UseBasicParsing -TimeoutSec 5 -Method Head
    Write-Host "OK  index.json $($probe.RawContentLength) bytes, $zipName HTTP $($zipProbe.StatusCode)"
    Stop-Job $job; Remove-Job $job -Force
    exit 0
}

Write-Host ""
Write-Host "本地市场已就绪：$baseUrl" -ForegroundColor Green
Write-Host "  Phase $Phase（latest = $latest）"
Write-Host ""
Write-Host "启动程序（另开终端，或在当前终端里执行）：" -ForegroundColor Yellow
Write-Host "  `$env:ASSISTANT_MARKET_URL = '$baseUrl/index.json'"
Write-Host "  .\build\Release\run.bat"
Write-Host ""
Write-Host "演示顺序：" -ForegroundColor Yellow
Write-Host "  1. 打开「插件市场」→ 可安装组里能看到「文本工具」（来自本地索引，不是示例数据）"
Write-Host "  2. 点安装 → 确认框（权限 + 信任提示）→ 安装完成"
Write-Host "  3. 重启程序，左侧出现「文本工具」页面"
if ($Phase -eq 1) {
    Write-Host "  4. 想演示「更新」：重跑 .\tools\demo-market.ps1 -Phase 2 -Launch" -ForegroundColor DarkGray
}
Write-Host ""
Write-Host "服务在前台运行，按 Ctrl+C 结束。" -ForegroundColor DarkGray

if ($Launch) {
    $env:ASSISTANT_MARKET_URL = "$baseUrl/index.json"
    $runBat = Join-Path $RepoRoot 'build\Release\run.bat'
    if (Test-Path $runBat) { Start-Process -FilePath $runBat }
    else { Write-Warning "找不到 $runBat，先编译一次" }
}

Wait-Job $job | Out-Null
