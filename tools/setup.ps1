param(
    [switch]$WithPiper
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$config = Join-Path $root "BenshiNode.ini"

if (-not (Test-Path $config)) {
    Copy-Item (Join-Path $root "config\example.ini") $config
    Write-Host "Created BenshiNode.ini from the example."
    Write-Host "Edit RADIO/MAC and NODE/CALLSIGN before starting."
} else {
    Write-Host "Keeping existing BenshiNode.ini."
}

if ($WithPiper) {
    & (Join-Path $PSScriptRoot "install-piper.ps1") `
        -Destination (Join-Path $PSScriptRoot "piper")
}

Write-Host ""
Write-Host "Start the GUI with:"
Write-Host "  .\build\Release\BenshiNodeGUI.exe"
Write-Host "or:"
Write-Host "  .\build\Release\BenshiNode.exe --gui"
