# Build su Windows: l'equivalente di build.sh. Serve Visual Studio 2022 (workload C++), il
# Vulkan SDK (glslc), Python 3 e Git. Uso: .\build.ps1 [-Config Release|Debug]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# Il cmake nel PATH puo' essere vecchio (quello di VS 2019 non conosce VS 2022): si preferisce
# quello che arriva con l'ultima installazione di Visual Studio.
$cmake = "cmake"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vsCmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if ($vs -and (Test-Path $vsCmake)) { $cmake = $vsCmake }
}

Write-Host "Building the project..."
& $cmake -S . -B build -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $cmake --build build --config $Config
exit $LASTEXITCODE
