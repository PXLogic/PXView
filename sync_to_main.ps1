<#
.SYNOPSIS
    将 view_and_data 的当前最终状态,作为一个快照提交同步到 opensource/main。
    提交信息交互式输入:直接弹出系统默认编辑器(和 git commit 不带 -m 体验一致)。

.DESCRIPTION
    做法:在 opensource/main 最新节点后新增一个提交 N,N 的树 = view_and_data 当前状态。
    不引入 view_and_data 的历史提交。冲突时直接以 view_and_data 覆盖(无冲突提示)。
    main 上 view_and_data 没有的文件(如发布文档)会被保留。

    编辑器选择优先级:
      1. $env:GIT_EDITOR
      2. git config core.editor
      3. $env:EDITOR
      4. 记事本(notepad)  <- Windows 默认

.PARAMETER SourceBranch
    源分支名,默认 view_and_data。

.PARAMETER TargetRemote
    目标 remote,默认 opensource。

.PARAMETER TargetBranch
    目标分支,默认 main。

.PARAMETER TempBranch
    本地临时分支名,默认 sync_tmp。

.PARAMETER DryRun
    只展示将要执行的操作,不实际提交/推送。

.PARAMETER KeepBranch
    推送后保留本地临时分支(默认删除)。

.EXAMPLE
    .\sync_to_main.ps1
    .\sync_to_main.ps1 -DryRun
    .\sync_to_main.ps1 -SourceBranch my_feature -TargetBranch main
#>

[CmdletBinding()]
param(
    [string]$SourceBranch = "view_and_data",
    [string]$TargetRemote = "opensource",
    [string]$TargetBranch = "main",
    [string]$TempBranch = "sync_tmp",
    [switch]$DryRun,
    [switch]$KeepBranch
)

$ErrorActionPreference = "Stop"

function Run-Git {
    param([string]$Cmd, [switch]$IgnoreError)
    Write-Host ">> git $Cmd" -ForegroundColor Cyan
    if ($DryRun) { return }
    try {
        Invoke-Expression "git $Cmd" | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0 -and -not $IgnoreError) {
            throw "git $Cmd 失败 (exit $LASTEXITCODE)"
        }
    } catch {
        if ($IgnoreError) { return }
        throw $_
    }
}

# 1. 记录当前分支,以便最后切回
$origBranch = (git rev-parse --abbrev-ref HEAD).Trim()
Write-Host "当前分支: $origBranch" -ForegroundColor Yellow

# 2. 拉取目标 remote 最新
Run-Git "fetch $TargetRemote"

# 3. 若临时分支已存在则删掉重建
$existing = git branch --list $TempBranch
if ($existing) {
    Write-Host "临时分支 $TempBranch 已存在,删除重建" -ForegroundColor Yellow
    Run-Git "branch -D $TempBranch"
}

# 4. 基于目标分支最新节点建临时分支
Run-Git "checkout -b $TempBranch $TargetRemote/$TargetBranch"

# 5. 用源分支的树覆盖工作区(只覆盖源有的文件,源没有的文件保留)
Run-Git "checkout $SourceBranch -- ."

# 6. 暂存全部变更
Run-Git "add -A"

# 7. 展示将要提交的内容规模
Write-Host "待提交变更文件数:" -ForegroundColor Yellow -NoNewline
$cachedCount = (git diff --cached --name-only | Measure-Object -Line).Lines
Write-Host " $cachedCount" -ForegroundColor Yellow

# 8. 检查是否有变更可提交
$hasChanges = $false
if (-not $DryRun) {
    $statusOut = git status --porcelain
    if ($statusOut) { $hasChanges = $true }
} else {
    $hasChanges = $true  # DryRun 假设有
}

if (-not $hasChanges) {
    Write-Host "无变更可提交,跳过 commit/push,切回原分支。" -ForegroundColor Yellow
    Run-Git "checkout $origBranch"
    Run-Git "branch -D $TempBranch"
    return
}

# 9. 提交 —— 直接弹编辑器(和 git commit 不带 -m 体验一致)
if ($DryRun) {
    Write-Host "[DryRun] 跳过 commit(将弹出编辑器输入提交信息)" -ForegroundColor Yellow
} else {
    Write-Host "即将弹出编辑器,请在编辑器中输入提交信息,保存关闭后继续..." -ForegroundColor Green
    Write-Host "(编辑器选择优先级: GIT_EDITOR -> git config core.editor -> EDITOR -> notepad)" -ForegroundColor DarkGray

    # 直接调用 git commit(不带 -m),git 会自动弹编辑器并处理 GIT_EDITOR/core.editor
    # 等待编辑器关闭后,git 返回,提交完成
    git commit
    if ($LASTEXITCODE -ne 0) {
        Write-Host "提交未完成(编辑器退出码非0或无信息),放弃操作。" -ForegroundColor Red
        Run-Git "checkout $origBranch"
        Run-Git "branch -D $TempBranch"
        return
    }
}

# 10. 展示新提交
Write-Host "新提交:" -ForegroundColor Green
Run-Git "log --oneline -1"

# 11. 推送到目标分支(force-with-lease 安全强推)
Write-Host "推送 $TempBranch -> $TargetRemote/$TargetBranch (force-with-lease)" -ForegroundColor Yellow
Run-Git "push --force-with-lease $TargetRemote ${TempBranch}:${TargetBranch}"

# 12. 切回原分支并(可选)删除临时分支
Run-Git "checkout $origBranch"
if (-not $KeepBranch) {
    Run-Git "branch -D $TempBranch"
}

Write-Host "完成。$TargetBranch 已同步为 $SourceBranch 的最终状态。" -ForegroundColor Green
