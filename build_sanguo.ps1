# build_sanguo.ps1 —— sanguo_duel 固件构建脚本（固化工作流）。
# 背景：ninja 的 .ninja_deps 会记录 GSP 生成文件的 /D:/ 风格路径，增量构建时
# FindFirstFileExA 报错。先自检计数，非 0 才移走（保住增量的机会）。
param(
    [switch]$Clean,       # 额外清理整个 build 目录
    [switch]$KeepDeps     # 失败重跑用：不动 .ninja_deps
)

$ErrorActionPreference = "Stop"
$workspace = "D:\esp-mo\esp-mosaico-vibe-main"
$project = "projects/sanguo_duel"
$build = "$workspace\projects\sanguo_duel\build"
$idf = "D:\esp-mo\esp-idf"
$script = "$workspace\submodule\esp-mosaico-utils\mosaico-tools\skills\idf-low-noise-build\scripts\idf_low_noise_build.py"

if ($Clean -and (Test-Path $build)) {
    Write-Host "[build] clean: removing $build"
    Remove-Item $build -Recurse -Force
}

if (-not $KeepDeps -and (Test-Path "$build\.ninja_deps")) {
    $cnt = 0
    try {
        $bytes = [System.IO.File]::ReadAllBytes("$build\.ninja_deps")
        $text = [System.Text.Encoding]::ASCII.GetString($bytes)
        $cnt = ([regex]::Matches($text, "/D:/")).Count
    } catch { $cnt = 1 }
    if ($cnt -gt 0) {
        Write-Host "[build] .ninja_deps has $cnt /D:/ paths -> removing (full rebuild)"
        Remove-Item "$build\.ninja_deps" -Force
    } else {
        Write-Host "[build] .ninja_deps clean -> keeping incremental"
    }
}

Write-Host "[build] building $project ..."
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue  # Git Bash 下 IDF 导出会被拒
Push-Location $workspace
try {
    $env:IDF_PATH = $idf
    python $script --project $project build
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}
Write-Host "[build] OK"
