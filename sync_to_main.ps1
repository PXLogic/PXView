<#
.SYNOPSIS
    将 view_and_data 的当前最终状态同步到 opensource/main 工作区。
    只覆盖文件,不提交、不推送、不弹编辑器。

.DESCRIPTION
    做法:切到目标分支(默认 main),用 git checkout <source> -- . 把源分支
    的所有文件覆盖到工作区。覆盖后工作区处于"已修改未提交"状态,你可以
    自己 git diff 检查、git add、git commit。
    源分支没有但目标分支独有的文件会被保留(checkout -- . 不删除文件)。

.PARAMETER SourceBranch
    源分支名,默认 view_and_data。

.PARAMETER TargetBranch
    目标分支名(本地),默认 main。

.EXAMPLE
    .\sync_to_main.ps1
    .\sync_to_main.ps1 -SourceBranch view_and_data -TargetBranch main
#>

[CmdletBinding()]
param(
    [string]$SourceBranch = "view_and_data",
    [string]$TargetBranch = "main"
)

$ErrorActionPreference = "Stop"

# 1. 检查工作区是否干净
$status = git status --porcelain
if ($status) {
    Write-Host "工作区有未提交改动,请先 commit/stash 再运行此脚本。" -ForegroundColor Red
    Write-Host $status
    exit 1
}

# 2. 记录原分支
$origBranch = (git rev-parse --abbrev-ref HEAD).Trim()
Write-Host "当前分支: $origBranch" -ForegroundColor Yellow

# 3. 拉取目标 remote 最新并切到目标分支
Write-Host ">> git fetch opensource" -ForegroundColor Cyan
git fetch opensource | ForEach-Object { Write-Host $_ }

Write-Host ">> git checkout $TargetBranch" -ForegroundColor Cyan
git checkout $TargetBranch | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "切换到 $TargetBranch 失败" }

# 4. 用源分支的树覆盖工作区(不删除目标独有文件)
Write-Host ">> git checkout $SourceBranch -- ." -ForegroundColor Cyan
git checkout $SourceBranch -- .
if ($LASTEXITCODE -ne 0) { throw "checkout $SourceBranch 失败" }

# 5. 暂存(可选:不暂存则保持未跟踪状态,这里 add -A 让状态清晰)
Write-Host ">> git add -A" -ForegroundColor Cyan
git add -A | ForEach-Object { Write-Host $_ }

# 6. 展示变更概况
$changed = (git diff --cached --name-only | Measure-Object -Line).Lines
Write-Host ""
Write-Host "完成: $SourceBranch 的文件已同步到 $TargetBranch 工作区。" -ForegroundColor Green
Write-Host "已暂存变更文件数: $changed" -ForegroundColor Yellow
Write-Host ""
Write-Host "下一步你可以:" -ForegroundColor DarkGray
Write-Host "  git diff --cached        # 检查差异" -ForegroundColor DarkGray
Write-Host "  git commit -m '...'       # 自己提交" -ForegroundColor DarkGray
Write-Host "  git push opensource main  # 自己推送" -ForegroundColor DarkGray
Write-Host "  git reset --hard HEAD     # 放弃此次覆盖" -ForegroundColor DarkGray
