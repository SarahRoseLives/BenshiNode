param(
    [string]$Destination = (Join-Path $PSScriptRoot "piper"),
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$piperVersion = "2023.11.14-2"
$piperUrl = "https://github.com/rhasspy/piper/releases/download/$piperVersion/piper_windows_amd64.zip"
$modelBase = "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium"
$modelName = "en_US-lessac-medium"
$modelDir = Join-Path $Destination "voices"

New-Item -ItemType Directory -Force -Path $Destination, $modelDir | Out-Null

if ($Force -or -not (Test-Path (Join-Path $Destination "piper.exe"))) {
    $tempRoot = Join-Path $env:TEMP ("benshinode-piper-" + [guid]::NewGuid().ToString("N"))
    $archive = Join-Path $tempRoot "piper.zip"
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    try {
        Write-Host "Downloading Piper..."
        Invoke-WebRequest -Uri $piperUrl -OutFile $archive -UseBasicParsing
        Expand-Archive -Path $archive -DestinationPath $tempRoot -Force
        $piperExe = Get-ChildItem $tempRoot -Recurse -Filter "piper.exe" |
            Select-Object -First 1
        if (-not $piperExe) {
            throw "piper.exe was not found in the downloaded archive"
        }
        Get-ChildItem $piperExe.Directory |
            Copy-Item -Destination $Destination -Recurse -Force
    } finally {
        Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$modelFile = Join-Path $modelDir "$modelName.onnx"
$configFile = Join-Path $modelDir "$modelName.onnx.json"
if ($Force -or -not (Test-Path $modelFile)) {
    Write-Host "Downloading $modelName voice..."
    Invoke-WebRequest -Uri "$modelBase/$modelName.onnx?download=true" `
        -OutFile $modelFile -UseBasicParsing
}
if ($Force -or -not (Test-Path $configFile)) {
    Invoke-WebRequest -Uri "$modelBase/$modelName.onnx.json?download=true" `
        -OutFile $configFile -UseBasicParsing
}

& (Join-Path $Destination "piper.exe") --help | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Piper failed to start"
}

$relative = Resolve-Path -Relative $Destination
Write-Host ""
Write-Host "Piper installed. Add this to BenshiNode.ini:"
Write-Host ""
Write-Host "[PIPER]"
Write-Host "ENABLED=1"
Write-Host "EXE=$relative\piper.exe"
Write-Host "MODEL=$relative\voices\$modelName.onnx"
Write-Host "CACHE_DIR=tts-cache"
Write-Host "LENGTH_SCALE=1.0"
Write-Host "SENTENCE_SILENCE=0.05"
Write-Host "SPELL_CALLSIGN=1"
Write-Host "STARTUP_PROMPT=1"
