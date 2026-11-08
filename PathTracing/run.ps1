# Build e avvio su Windows: l'equivalente di run.sh. Uso: .\run.ps1 [-Config Release|Debug]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

& "$PSScriptRoot\build.ps1" -Config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running the project..."
# Lo stesso path di Linux: CMake toglie la sottocartella per configurazione anche con Visual Studio
& "$PSScriptRoot\build\bin\Lucerna.exe"
