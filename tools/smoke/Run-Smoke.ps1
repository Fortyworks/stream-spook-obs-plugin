# OBS を起動せずに、プラグインを libobs（D3D11）へ読み込んで確かめる。
#
# やること:
#   1. .deps に落としてある OBS のソースから libobs-d3d11 を組む（初回だけ数十秒）
#   2. smoke.c を cl で組む
#   3. 帯の計算（spectrum-analyzer.c）を libobs 無しで確かめる
#   4. プラグインを読み込み、全フィルタを作って（= .effect をコンパイルして）
#      単色のソースに掛け、ピクセルを読み戻す。オーディオビジュアライザーは
#      偽の obs-websocket（vendor API）を先に置いて、購読・音・期限まで見る
#
# 前提: `cmake --preset windows-x64` と `cmake --build --preset windows-x64` が
# 済んでいること（.deps と build_x64 があること）。
#
# 使い方: pwsh obs-plugin/tools/smoke/Run-Smoke.ps1
# 終了コード 0 なら OK。シェーダーの警告・エラーは出力に [obs 300] 以下で出る。

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$spec = Get-Content (Join-Path $root "buildspec.json") -Raw | ConvertFrom-Json
$obsVer = $spec.dependencies."obs-studio".version
$depsVer = $spec.dependencies.prebuilt.version

$deps = Join-Path $root ".deps"
$obsSrc = Join-Path $deps "obs-studio-$obsVer"
$obsBuild = Join-Path $obsSrc "build_x64"
$runBin = Join-Path $obsBuild "rundir\Release\bin\64bit"
$runData = Join-Path $obsBuild "rundir\Release\data\libobs"
$prebuilt = Join-Path $deps "obs-deps-$depsVer-x64\bin"
$pluginDll = Join-Path $root "build_x64\RelWithDebInfo\stream-spook.dll"
$pluginData = Join-Path $root "build_x64\rundir\RelWithDebInfo\stream-spook"

foreach ($p in @($obsBuild, $prebuilt, $pluginDll, $pluginData)) {
    if (-not (Test-Path $p)) {
        throw "not found: $p  (先に cmake --preset windows-x64 / --build --preset windows-x64 を回す)"
    }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio (C++ ツール) が見つからない" }
$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

# 1. libobs-d3d11（OBS 本体のビルドでは libobs しか組んでいない）
$d3d = Join-Path $obsBuild "libobs-d3d11\Release\libobs-d3d11.dll"
if (-not (Test-Path $d3d)) {
    Write-Host "== build libobs-d3d11"
    & $cmake --build $obsBuild --target libobs-d3d11 --config Release --parallel | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "libobs-d3d11 のビルドに失敗" }
}
Copy-Item $d3d $runBin -Force

# 2. smoke.exe
$out = Join-Path $root "build_x64\smoke"
New-Item -ItemType Directory -Force $out | Out-Null
$bat = Join-Path $out "build.bat"
@"
@echo off
rem vcvars64.bat が中で vswhere を呼ぶ。PATH に無いと警告を吐くので先に足す
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "$out"
cl /nologo /W3 /wd4996 /utf-8 /I "$deps\include" /I "$root\src" "$PSScriptRoot\smoke.c" /link "$deps\lib\obs.lib" /OUT:smoke.exe
"@ | Set-Content -Encoding ascii $bat
Write-Host "== build smoke.exe"
cmd /c $bat
if ($LASTEXITCODE -ne 0) { throw "smoke.exe のビルドに失敗" }

# 3. 帯の計算だけを libobs 無しで確かめる（CMake の target。配布物には入らない）
Write-Host "== analyzer test"
& $cmake --build (Join-Path $root "build_x64") --config RelWithDebInfo --target spectrum-analyzer-test | Out-Null
if ($LASTEXITCODE -ne 0) { throw "spectrum-analyzer-test のビルドに失敗" }
& (Join-Path $root "build_x64\RelWithDebInfo\spectrum-analyzer-test.exe")
if ($LASTEXITCODE -ne 0) { throw "spectrum-analyzer-test が失敗" }

# 4. run
Write-Host "== run"
$env:PATH = "$prebuilt;$runBin;$env:PATH"
Push-Location $runBin
try {
    & (Join-Path $out "smoke.exe") "$runData\" $pluginDll $pluginData
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
exit $code
