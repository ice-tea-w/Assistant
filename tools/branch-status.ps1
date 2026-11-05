#!/usr/bin/env pwsh
#requires -Version 5.1
<#
.SYNOPSIS
    对表 main 与各远程分支：是否已合并、领先/落后多少、试合并有哪些冲突。

.DESCRIPTION
    只读工具（除默认的 fetch）。给"分支又更新了，怎么合"这个问题一个可复现的答案，
    避免靠记忆判断 merge-base 和冲突范围。

    输出列：
      分支        远程分支名（origin/xxx）
      领先        分支上不在 main 的提交数
      落后        main 上不在分支的提交数
      基线        merge-base 的短 hash 与日期 —— 它比分支 tip 老很多，就是"历史被改写"的信号
      状态        已合并 / 可合并（无冲突） / 冲突 N
      最新提交    分支 tip 的标题

    判定规则见 docs/architecture/分支整合流程.md §3。

.EXAMPLE
    pwsh tools/branch-status.ps1
    pwsh tools/branch-status.ps1 -NoFetch
    pwsh tools/branch-status.ps1 -FailOnConflict   # 有冲突时 exit 1，可挂到 CI/定时任务

.NOTES
    本机 git 的 schannel 后端时常拿不到凭据，fetch 默认显式走 openssl；
    失败只警告，不中断（可用 -NoFetch 完全跳过网络）。
#>
[CmdletBinding()]
param(
    [string]$Base = 'main',
    [string]$Remote = 'origin',
    [switch]$NoFetch,
    [switch]$FailOnConflict
)

$ErrorActionPreference = 'Stop'

if (-not $NoFetch) {
    Write-Host "fetch $Remote ..." -ForegroundColor DarkGray
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & git -c http.sslBackend=openssl fetch $Remote --prune 2>&1 |
        ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    $ErrorActionPreference = $prev
}

& git rev-parse --verify --quiet $Base *> $null
if ($LASTEXITCODE -ne 0) {
    throw "找不到基线分支 '$Base'（先 git switch $Base 或检查参数）"
}

$rows = @()
$conflictDetails = @{}

$branches = & git for-each-ref --format='%(refname:short)' "refs/remotes/$Remote" |
    Where-Object { $_ -and $_ -ne $Remote -and $_ -ne "$Remote/HEAD" -and $_ -ne "$Remote/$Base" } |
    Sort-Object

if (-not $branches) {
    Write-Warning "没有找到 $Remote 下的远程分支（先 fetch？）"
    exit 0
}

foreach ($br in $branches) {
    $ahead = [int](& git rev-list --count "$Base..$br")
    $behind = [int](& git rev-list --count "$br..$Base")

    $baseSha = (& git merge-base $Base $br).Trim()
    $baseInfo = (& git log -1 --format='%h %ad' --date=short $baseSha).Trim()

    & git merge-base --is-ancestor $br $Base 2>$null
    $merged = ($LASTEXITCODE -eq 0)

    $status = ''
    $conflicts = @()
    if ($merged) {
        $status = '已合并'
    }
    else {
        $out = @(& git merge-tree --write-tree --name-only --no-messages $Base $br 2>$null)
        if ($LASTEXITCODE -eq 0) {
            $status = '可合并（无冲突）'
        }
        else {
            # 第 1 行是结果 tree 的 OID，其余是冲突文件
            $conflicts = @($out | Select-Object -Skip 1 | Where-Object { $_ })
            $status = "冲突 $($conflicts.Count)"
            $conflictDetails[$br] = $conflicts
        }
    }

    $subject = (& git log -1 --format='%s' $br)

    $rows += [pscustomobject]@{
        分支     = $br
        领先     = $ahead
        落后     = $behind
        基线     = $baseInfo
        状态     = $status
        最新提交 = $subject
    }
}

$rows | Format-Table -AutoSize -Wrap

foreach ($br in $conflictDetails.Keys | Sort-Object) {
    Write-Host "`n[$br] 冲突文件：" -ForegroundColor Yellow
    $conflictDetails[$br] | ForEach-Object { Write-Host "  $_" }
}

# 给下一步动作一句提示
$mergedList = @($rows | Where-Object { $_.状态 -eq '已合并' })
$conflictList = @($rows | Where-Object { $_.状态 -like '冲突*' })
Write-Host ""
Write-Host ("已合并 {0} 条 · 冲突 {1} 条 · 其余 {2} 条" -f `
        $mergedList.Count, $conflictList.Count, ($rows.Count - $mergedList.Count - $conflictList.Count)) `
    -ForegroundColor DarkGray
if ($mergedList.Count -gt 0) {
    Write-Host "已合并的分支：可打 archive/<name> tag 后删除远程分支，不要再 merge。" -ForegroundColor DarkGray
}
if ($conflictList.Count -gt 0) {
    Write-Host "有冲突的分支：在分支上 git merge origin/$Base 解决，不要在 main 上解。" -ForegroundColor DarkGray
}

if ($FailOnConflict -and $conflictList.Count -gt 0) { exit 1 }
