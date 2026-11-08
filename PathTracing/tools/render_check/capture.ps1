# Cattura deterministica: PNG, HDR lineare e tempi GPU per ogni scena. Equivalente Windows di
# capture.sh. Uso: capture.ps1 <cartella-output> [scene...]
# Ambiente: FRAMES (default 600), piu' qualunque PT_* da passare all'app.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Scenes
)
if (-not $Scenes) { $Scenes = @("2", "5", "6", "8") }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path
# l'eseguibile sta in build\bin e trova da se' i suoi dati; con PT_SHOT non tocca il layout dell'utente
$app = if ($env:LUCERNA) { $env:LUCERNA } else { Join-Path $PSScriptRoot "..\..\build\bin\Lucerna.exe" }
$frames = if ($env:FRAMES) { $env:FRAMES } else { "600" }
$viewport = if ($env:PT_VIEWPORT) { $env:PT_VIEWPORT } else { "1280x720" }

foreach ($s in $Scenes) {
    $log = Join-Path $Out "s$s.log"
    $vars = @{ PT_NOVSYNC = "1"; PT_VIEWPORT = $viewport; PT_SCENE = $s; PT_SHOT = "$Out\s$s.png";
               PT_HDR = "$Out\s$s.hdr"; PT_SHOT_FRAME = $frames; PT_LOG_FPS = "1" }
    $saved = @{}
    foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $vars[$k]) }
    try {
        $p = Start-Process -FilePath $app -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
        $null = $p.Handle # senza leggere l'handle PowerShell 5.1 perde l'ExitCode
        if (-not $p.WaitForExit(240000)) { $p.Kill(); $p.WaitForExit() }
        $rc = $p.ExitCode
    }
    finally {
        foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
    Get-Content "$log.err" | Add-Content $log
    Remove-Item "$log.err"

    # Righe "PTFPS <fps> pt_ms <ms> ...": la prima e' esclusa perche' copre il caricamento della scena
    $fps = @(Select-String -Path $log -Pattern '^PTFPS' | ForEach-Object { , ($_.Line -split '\s+') })
    $ms = @($fps | ForEach-Object { [double]$_[3] } | Sort-Object)
    $median = if ($ms.Count) { $ms[[int][math]::Floor(($ms.Count - 1) / 2)] } else { "" }
    # sotto 5 FPS il compositor sta rallentando la finestra coperta: tempi non validi
    $slow = @($fps | Select-Object -Skip 1 | Where-Object { [double]$_[1] -lt 5 }).Count
    $errors = @(Select-String -Path $log -Pattern 'ERROR').Count
    Write-Host "scena $s rc=$rc pt_ms=$median throttled=$slow errori=$errors"
}
