<#
.SYNOPSIS
    把一个插件目录打成可分发的 zip，并生成对应的市场索引条目。

.DESCRIPTION
    只做三件事，每件都对应宿主安装器真实会检查的东西：
      1. 校验清单（字段/版本/入口文件），避免打出一个装不上的包；
      2. zip 的根必须是 plugin.json（不能多套一层目录）；
      3. 算 sha256 与字节数 —— 安装器会比对，对不上直接拒装。

    插件运行产生的 data/ 目录不会被打进包。

.PARAMETER BaseUrl
    下载地址前缀，例如 https://github.com/you/repo/releases/download/v1。
    省略时 url 字段填占位符，需要自己补。

.EXAMPLE
    .\tools\plugin-pack.ps1 -PluginDir .\examples\text-tools -OutDir .\dist

.EXAMPLE
    .\tools\plugin-pack.ps1 -PluginDir .\examples\text-tools -OutDir .\dist -BaseUrl "https://github.com/me/assistant-plugins/releases/download/1.0.0" -Categories 工具,文本
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $PluginDir,
    [string] $OutDir = ".\dist",
    [string] $BaseUrl = "",
    [string[]] $Categories = @("工具"),
    [string] $Changelog = "",
    [switch] $NoEntryFile
)

$ErrorActionPreference = 'Stop'

# -Categories 从命令行传进来时可能是一个 "工具,文本" 字符串，也可能是真数组；
# 统一按逗号拆开，省得作者被绑定规则坑。
$Categories = @($Categories | ForEach-Object { $_ -split ',' } | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
if ($Categories.Count -eq 0) { $Categories = @('工具') }

$PluginDir = (Resolve-Path $PluginDir).Path
$manifestPath = Join-Path $PluginDir 'plugin.json'

if (-not (Test-Path $manifestPath)) {
    throw "找不到清单：$manifestPath"
}

# ---- 1. 校验清单 ----
try {
    $m = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
}
catch {
    throw "plugin.json 不是合法 JSON：$($_.Exception.Message)"
}

foreach ($field in 'manifestVersion', 'id', 'name', 'version', 'main') {
    if (-not $m.PSObject.Properties[$field] -or [string]::IsNullOrWhiteSpace([string]$m.$field)) {
        throw "清单缺少必填字段：$field"
    }
}
if ($m.manifestVersion -ne 1) { throw "manifestVersion 必须是 1（当前 $($m.manifestVersion)）" }
if ($m.id -notmatch '^[a-z0-9][a-z0-9._-]{2,63}$') { throw "id 非法：$($m.id)" }
if ($m.version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') {
    throw "version 必须是严格 SemVer（如 1.0.0 或 1.2.0-beta.1），当前：$($m.version)"
}
if ($m.runtime -and $m.runtime -ne 'qml') {
    throw "runtime=$($m.runtime) 在 v1 不支持（只支持 qml）"
}
$mainPath = Join-Path $PluginDir $m.main
if (-not (Test-Path $mainPath)) { throw "清单里的 main 指向的文件不存在：$($m.main)" }

# ---- 2. 打包（根目录必须是 plugin.json）----
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("assistant-pack-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null

try {
    Copy-Item -Path (Join-Path $PluginDir '*') -Destination $stage -Recurse -Force
    foreach ($junk in 'data', '.trash', '.cache') {
        $p = Join-Path $stage $junk
        if (Test-Path $p) { Remove-Item $p -Recurse -Force }
    }

    if (-not (Test-Path (Join-Path $stage 'plugin.json'))) {
        throw "打包失败：staging 根目录没有 plugin.json"
    }

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $zipName = "$($m.id)-$($m.version).zip"
    $zipPath = Join-Path (Resolve-Path $OutDir).Path $zipName
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal
}
finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue }
}

# ---- 3. 摘要与索引条目 ----
$sha  = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $zipPath).Length

$url = if ($BaseUrl) { ($BaseUrl.TrimEnd('/')) + "/" + $zipName } else { "REPLACE_ME/$zipName" }

$entry = [ordered]@{
    id          = $m.id
    name        = $m.name
    summary     = $m.description
    description = $m.description
    author      = $m.author
    categories  = @($Categories)
    tags        = @()
    icon        = $m.icon
    latest      = [ordered]@{
        version       = $m.version
        url           = $url
        sha256        = $sha
        size          = $size
        minAppVersion = if ($m.engines -and $m.engines.assistant) { $m.engines.assistant } else { "0.3.0" }
        changelog     = $Changelog
    }
    updatedAt   = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
}

# PS 5.1 会把 > / < / & 转成 \u003e 之类的转义（JSON 合法但难读），转回来。
$entryJson = ($entry | ConvertTo-Json -Depth 10).Replace('\u003e', '>').Replace('\u003c', '<').Replace('\u0026', '&')

if (-not $NoEntryFile) {
    $entryPath = Join-Path (Resolve-Path $OutDir).Path "$($m.id)-$($m.version).entry.json"
    [System.IO.File]::WriteAllText($entryPath, $entryJson + "`n", [System.Text.UTF8Encoding]::new($false))
}

Write-Host ""
Write-Host "已打包" -ForegroundColor Green
Write-Host "  zip    : $zipPath"
Write-Host "  size   : $size 字节"
Write-Host "  sha256 : $sha"
Write-Host ""
Write-Host "市场索引条目（贴进 index.json 的 plugins 数组）："
Write-Host $entryJson
Write-Host ""
if (-not $BaseUrl) {
    Write-Warning "url 是占位符。重跑一次并加 -BaseUrl 你的下载前缀，或手工替换 REPLACE_ME。"
}
Write-Host "验证安装：把 ASSISTANT_MARKET_URL 指向本地 index.json，或在「插件市场」里直接侧载这个 zip。"
