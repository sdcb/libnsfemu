param(
    [Parameter(Mandatory = $true)]
    [string]$CompareExe,

    [Parameter(Mandatory = $true)]
    [string]$NsfDirectory,

    [int]$Seconds = 2
)

$ErrorActionPreference = "Stop"
$rates = @(8000, 36000, 44100, 48000)
$files = Get-ChildItem -LiteralPath $NsfDirectory -Filter "*.nsf" | Sort-Object Name

if ($files.Count -eq 0) {
    throw "No NSF files found in $NsfDirectory"
}

foreach ($rate in $rates) {
    foreach ($file in $files) {
        Write-Host "compare rate=$rate file=$($file.Name)"
        & $CompareExe $file.FullName 0 $rate $Seconds
        if ($LASTEXITCODE -ne 0) {
            throw "Comparison failed for $($file.FullName) at $rate Hz"
        }
    }
}

Write-Host "All local NSF comparisons passed."
