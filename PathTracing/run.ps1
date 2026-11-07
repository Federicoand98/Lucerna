# Build e avvio su Windows: l'equivalente di run.sh. Uso: .\run.ps1 [-Config Release|Debug]
param([string]$Config = "Release")
$ErrorActionPreference = "Stop"

& "$PSScriptRoot\build.ps1" -Config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Running the project..."
# Il generatore di Visual Studio e' multi-config: l'eseguibile sta in build\bin\<Config>
& "$PSScriptRoot\build\bin\$Config\Lucerna.exe"
