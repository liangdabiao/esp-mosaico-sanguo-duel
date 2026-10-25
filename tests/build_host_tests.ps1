# build_host_tests.ps1 -- sanguo_duel 宿主单测（zig cc）。
# 编译并运行与设备端完全相同的纯 C 逻辑层（main/sd/）。
# 用法: powershell -ExecutionPolicy Bypass -File build_host_tests.ps1
$ErrorActionPreference = "Stop"

$TestsDir = $PSScriptRoot
$ProjectDir = Split-Path -Parent $TestsDir
$SdDir = Join-Path $ProjectDir "main\sd"
$BuildDir = Join-Path $TestsDir "build"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$zig = "C:\Users\49707\AppData\Local\Microsoft\WinGet\Packages\zig.zig_Microsoft.Winget.Source_8wekyb3d8bbwe\zig-x86_64-windows-0.17.0\zig.exe"
if (-not (Test-Path $zig)) {
    $gcc = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gcc) {
        Write-Host "[tests] zig not found, fallback gcc $($gcc.Source)"
        & $gcc.Source -std=c11 -Wall -Wextra -I $SdDir `
            (Join-Path $SdDir "sd_cards.c") (Join-Path $SdDir "sd_duel.c") (Join-Path $SdDir "sd_ai.c") `
            (Join-Path $TestsDir "test_sd_duel.c") -o (Join-Path $BuildDir "test_sd_duel.exe")
    } else {
        throw "no zig or gcc found"
    }
} else {
    Write-Host "[tests] zig cc"
    & $zig cc -std=c11 -Wall -Wextra -I $SdDir `
        (Join-Path $SdDir "sd_cards.c") (Join-Path $SdDir "sd_duel.c") (Join-Path $SdDir "sd_ai.c") `
        (Join-Path $TestsDir "test_sd_duel.c") -o (Join-Path $BuildDir "test_sd_duel.exe")
}
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

& (Join-Path $BuildDir "test_sd_duel.exe")
exit $LASTEXITCODE
