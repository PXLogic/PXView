#!/usr/bin/env pwsh
# Run clang-tidy on PXView source files (not third-party libs)
# Outputs results to clang-tidy-results.txt

$ErrorActionPreference = "Continue"
$projectRoot = "c:\Users\admin\Downloads\Downloads\DSView-main_2026_4_27cppnb"
$buildDir = "$projectRoot\build"
$outputFile = "$projectRoot\clang-tidy-results.txt"

# Extra args to make clang-tidy work with MinGW GCC compile commands
$extraArgs = @(
    "--extra-arg=-target",
    "--extra-arg=x86_64-w64-mingw32",
    "--extra-arg=--gcc-toolchain=C:/msys64/mingw64",
    "--extra-arg=-Wno-ignored-gch",
    "--extra-arg=-Wno-unused-command-line-argument"
)

# Get list of PXView source files from compile_commands.json
$j = Get-Content "$buildDir\compile_commands.json" -Raw | ConvertFrom-Json
$pxviewFiles = $j | Where-Object {
    $_.file -like "*/PXView/*.cpp" -and
    $_.file -notlike "*/moc_*" -and
    $_.file -notlike "*/qrc_*" -and
    $_.file -notlike "*/cmake_pch*"
} | ForEach-Object { $_.file }

Write-Host "Found $($pxviewFiles.Count) PXView source files to check"
Write-Host "Output will be saved to: $outputFile"
Write-Host ""

# Clear output file
"" | Out-File $outputFile -Encoding utf8

# Run clang-tidy on each file
$count = 0
$total = $pxviewFiles.Count
foreach ($file in $pxviewFiles) {
    $count++
    $relativePath = $file -replace [regex]::Escape("$projectRoot/"), ""
    Write-Host "[$count/$total] $relativePath" -NoNewline

    $args = @("-p", $buildDir) + $extraArgs + @($file)
    $result = & clang-tidy @args 2>&1

    # Filter out non-user-code warnings
    $userWarnings = $result | Where-Object {
        $_ -notmatch "^Suppressed" -and
        $_ -notmatch "^Use -header-filter" -and
        $_ -notmatch "^warning: argument unused" -and
        $_ -notmatch "in non-user code" -and
        $_ -notmatch "^$"
    }

    if ($userWarnings) {
        $warningCount = ($userWarnings | Where-Object { $_ -match "warning:" }).Count
        $errorCount = ($userWarnings | Where-Object { $_ -match "error:" }).Count
        Write-Host " -> $warningCount warnings, $errorCount errors" -ForegroundColor Yellow

        "=== $relativePath ===" | Out-File $outputFile -Append -Encoding utf8
        $userWarnings | Out-File $outputFile -Append -Encoding utf8
        "" | Out-File $outputFile -Append -Encoding utf8
    } else {
        Write-Host " -> clean" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "Done! Results saved to: $outputFile"
