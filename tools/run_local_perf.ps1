param(
    [Parameter(Mandatory = $true)]
    [string]$CompareExe,

    [Parameter(Mandatory = $true)]
    [string]$NsfDirectory,

    [int]$SampleRate = 44100,
    [int]$Seconds = 120
)

$ErrorActionPreference = "Stop"
$files = Get-ChildItem -LiteralPath $NsfDirectory -Filter "*.nsf" | Sort-Object Name

if ($files.Count -eq 0) {
    throw "No NSF files found in $NsfDirectory"
}

$oursTotal = 0.0
$gmeTotal = 0.0

foreach ($file in $files) {
    Write-Host "perf file=$($file.Name)"
    $output = & $CompareExe $file.FullName 0 $SampleRate $Seconds
    if ($LASTEXITCODE -ne 0) {
        $output | Write-Host
        throw "Comparison failed for $($file.FullName)"
    }

    $output | Write-Host
    $line = $output | Where-Object { $_ -match '^timing_no_callback ' } | Select-Object -Last 1
    if (-not $line) {
        throw "No timing_no_callback line found for $($file.FullName)"
    }

    if ($line -notmatch 'ours=([0-9.]+)s gme=([0-9.]+)s') {
        throw "Unable to parse timing line: $line"
    }

    $oursTotal += [double]$Matches[1]
    $gmeTotal += [double]$Matches[2]
}

$ratio = if ($gmeTotal -gt 0.0) { $oursTotal / $gmeTotal } else { 0.0 }
Write-Host ("aggregate_timing_no_callback ours={0:N6}s gme={1:N6}s ratio={2:N3}" -f $oursTotal, $gmeTotal, $ratio)

if ($ratio -gt 1.0) {
    throw "Aggregate nsfemu time is slower than libgme."
}
