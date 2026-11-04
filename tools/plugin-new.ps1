<#
.SYNOPSIS
    生成一个 Assistant 插件骨架（plugin.json + main.qml + Page.qml + README）。

.DESCRIPTION
    给"我要写个插件"的第一步用。生成完复制到
    %LOCALAPPDATA%\AssistantTeam\Assistant\plugins\<id>\ 重启宿主即可看到效果。
    清单字段的权威说明见 docs/plugins/README.md。

.EXAMPLE
    .\tools\plugin-new.ps1 -Id com.example.hello -Name "Hello"

.EXAMPLE
    .\tools\plugin-new.ps1 -Id com.me.weather -Name "天气" -Icon "🌤" -OutDir D:\work -Author me
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Id,
    [Parameter(Mandatory)] [string] $Name,
    [string] $OutDir = ".",
    [string] $Author = "your-name",
    [string] $Icon = "🧩",
    [string] $Description = "描述你的插件做什么",
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

# 与 PluginManifest.cpp 的正则保持一致，不让作者先生成一个会被拒的 id。
if ($Id -notmatch '^[a-z0-9][a-z0-9._-]{2,63}$') {
    throw "id 非法：$Id  要求 ^[a-z0-9][a-z0-9._-]{2,63}$（建议反向域名，如 com.example.hello）"
}

$target = Join-Path $OutDir $Id
if ((Test-Path $target) -and -not $Force) {
    throw "目录已存在：$target（加 -Force 覆盖）"
}

$utf8 = [System.Text.UTF8Encoding]::new($false)
function Write-Utf8([string] $Path, [string] $Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $utf8)
}

New-Item -ItemType Directory -Path $target -Force | Out-Null

$manifest = [ordered]@{
    manifestVersion = 1
    id              = $Id
    name            = $Name
    version         = '0.1.0'
    description     = $Description
    author          = $Author
    license         = 'MIT'
    icon            = $Icon
    runtime         = 'qml'
    engines         = [ordered]@{
        assistant = '>=0.3.0'
        # 用了较新的 host 能力时把这个下限抬上去（docs/plugins/README.md 7.2）
        api       = '>=1.0.0'
    }
    main            = 'main.qml'
    # v1 只是声明、宿主不拦截；写真实需要的能力，方便用户判断
    permissions     = @()
    contributes     = [ordered]@{
        pages = @(
            [ordered]@{
                id     = 'main'
                title  = $Name
                icon   = $Icon
                source = 'Page.qml'
            }
        )
    }
}

# PS 5.1 的 ConvertTo-Json 会把 >= / < / & 转成 \u003e 之类的转义，
# 功能上合法但作者看着别扭，这里转回来（只影响这几个字符，不动其它内容）。
function New-PluginJson($Object) {
    $json = $Object | ConvertTo-Json -Depth 10
    return $json.Replace('\u003e', '>').Replace('\u003c', '<').Replace('\u0026', '&')
}

Write-Utf8 (Join-Path $target 'plugin.json') ((New-PluginJson $manifest) + "`n")

$mainQml = @"
// 插件入口：启用时实例化一次，之后一直存在。
// 页面是另外懒加载的（见 Page.qml），所以这里只放初始化和不依赖界面的逻辑。
import QtQml

QtObject {
    Component.onCompleted: {
        host.log("info", "$Name 已加载 · 宿主 " + host.appVersion + " · API " + host.apiVersion)
    }
}
"@
Write-Utf8 (Join-Path $target 'main.qml') $mainQml

$pageQml = @"
// 贡献给左侧导航的页面。
// 注意：宿主填满内容区，不要自己设 width/height；配色自己来（host.isDark）。
import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    readonly property bool dark: host.isDark
    color: dark ? "#2c2c2c" : "#f0f0f0"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Text {
            text: "$Name"
            font.pixelSize: 18
            font.bold: true
            color: dark ? "#e0e0e0" : "#1a1a1a"
        }

        Text {
            text: "插件 id：" + host.pluginId + " · 版本 " + host.version
            font.pixelSize: 12
            color: dark ? "#666666" : "#999999"
        }
    }
}
"@
Write-Utf8 (Join-Path $target 'Page.qml') $pageQml

$readme = @"
# $Name

跑起来（在仓库根目录执行）：

    Copy-Item -Recurse $target `$env:LOCALAPPDATA\AssistantTeam\Assistant\plugins\$Id

重启 Assistant，打开「插件市场」，打开开关，左侧会出现「$Name」。

打包侧载或发布：

    .\tools\plugin-pack.ps1 -PluginDir $target -OutDir .\dist

清单与 host API 说明见 docs/plugins/README.md。
"@
Write-Utf8 (Join-Path $target 'README.md') $readme

Write-Host ""
Write-Host "已生成：$target" -ForegroundColor Green
Get-ChildItem $target -File | ForEach-Object { Write-Host ("  " + $_.Name) }
Write-Host ""
Write-Host "下一步："
Write-Host "  1. 改 Page.qml 里的界面"
Write-Host "  2. 把目录复制到 %LOCALAPPDATA%\AssistantTeam\Assistant\plugins\"
Write-Host "  3. 重启 Assistant，在「插件市场」里打开开关"
