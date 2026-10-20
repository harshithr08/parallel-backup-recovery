<#
.SYNOPSIS
    Live demo of IPBRS: backup, damage, detection, block-level recovery, and
    file-level versus IPBRS scheduling, checked independently with Get-FileHash.

.DESCRIPTION
    Works in its own temporary folder and deletes it afterwards (unless -Keep).
    Needs about 3x the dataset size free on the temp drive (default ~3 GB).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File demo.ps1
    powershell -ExecutionPolicy Bypass -File demo.ps1 -Pause      # wait for Enter between steps
    powershell -ExecutionPolicy Bypass -File demo.ps1 -BigMiB 256 # smaller, faster dataset
#>
param(
    [int]$Files = 2000,
    [int]$BigFiles = 2,
    [int]$BigMiB = 512,
    [int]$Threads = [Environment]::ProcessorCount,
    [switch]$Pause,
    [switch]$Keep
)

$ErrorActionPreference = "Stop"
$exe = Join-Path $PSScriptRoot "build\ipbrs.exe"
if (-not (Test-Path $exe)) { throw "build\ipbrs.exe not found; run build.bat first" }

$work = Join-Path $env:TEMP ("ipbrs-demo-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$src = Join-Path $work "source"
$bk = Join-Path $work "backup"

$needBytes = 3 * ($BigFiles * $BigMiB * 1MB + $Files * 16KB)
$drive = (Get-Item $env:TEMP).PSDrive
if ($drive.Free -lt $needBytes) {
    throw ("need about {0:N1} GB free on {1}:, have {2:N1} GB" -f ($needBytes / 1GB), $drive.Name, ($drive.Free / 1GB))
}

function Invoke-Ipbrs {
    $output = & $exe @args
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 2) { throw "ipbrs $($args -join ' ') failed (exit $LASTEXITCODE)" }
    ($output | Where-Object { $_.StartsWith("{") } | Select-Object -Last 1) | ConvertFrom-Json
}

$step = 0
function Show-Step([string]$title) {
    $script:step++
    if ($Pause -and $script:step -gt 1) { Read-Host "`n  press Enter for the next step" | Out-Null }
    Write-Host ""
    Write-Host ("== Step {0}: {1} " -f $script:step, $title).PadRight(78, "=") -ForegroundColor Cyan
}
function Show([string]$label, $value) { Write-Host ("   {0,-34} {1}" -f $label, $value) }
function Good([string]$text) { Write-Host "   [OK] $text" -ForegroundColor Green }
function Bad([string]$text) { Write-Host "   [FAIL] $text" -ForegroundColor Red; $script:failures++ }
function Sec($ns) { "{0:N2} s" -f ($ns / 1e9) }
function MiB($bytes) { "{0:N1} MiB" -f ($bytes / 1MB) }

function Get-TreeHashes([string]$root) {
    $table = @{}
    Get-ChildItem -LiteralPath $root -Recurse -File | ForEach-Object {
        $table[$_.FullName.Substring($root.Length)] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
    $table
}

$failures = 0
try {
    Write-Host "IPBRS live demo - $Threads threads, working in $work" -ForegroundColor Yellow

    Show-Step "generate a heterogeneous dataset"
    $gen = Invoke-Ipbrs gen --out $src --files $Files --profile low --big-files $BigFiles --big-mib $BigMiB --seed 42
    Show "files" ("{0:N0} small (8-16 KiB) + {1} x {2} MiB" -f $Files, $BigFiles, $BigMiB)
    Show "total size" (MiB $gen.bytes)
    $before = Get-TreeHashes $src
    Show "independent fingerprint" "Get-FileHash SHA256 of all $($before.Count) files recorded"

    Show-Step "durable parallel backup (IPBRS: work stealing + block splitting)"
    $b = Invoke-Ipbrs backup --source $src --backup $bk --threads $Threads
    Show "files backed up" $b.files
    Show "time (flushed to disk)" (Sec $b.total_ns)
    Show "large files split into" ("{0} block tasks ({1} tasks in total)" -f ($b.plan.tasks - $b.plan.ranges + $b.plan.split_files), $b.plan.tasks)
    $v = Invoke-Ipbrs verify --backup $bk --threads $Threads
    if ($b.committed -and $v.bad -eq 0) { Good "backup committed and independently re-verified ($($v.ok) files)" } else { Bad "backup verification" }

    Show-Step "damage the source (what a disk fault or a mistake would do)"
    $d = Invoke-Ipbrs damage --target $src --ratio 0.05 --seed 7
    foreach ($kind in "missing", "modified", "corrupted", "truncated") { Show "  $kind" $d.by_type.$kind }
    $big = Get-ChildItem -LiteralPath (Join-Path $src "big") -File | Where-Object { $_.Length -gt 0 } | Select-Object -First 1
    if ($big) {
        $stamp = $big.LastWriteTimeUtc
        $stream = [IO.File]::Open($big.FullName, "Open", "ReadWrite")
        try { $stream.Position = [long](0.6 * $big.Length); $stream.WriteByte(0x5A) } finally { $stream.Close() }
        (Get-Item -LiteralPath $big.FullName).LastWriteTimeUtc = $stamp
        Show "  silent bit rot" ("1 byte inside {0} ({1:N0} MiB), timestamp unchanged" -f $big.Name, ($big.Length / 1MB))
    }

    Show-Step "detect the damage (dry run: nothing is changed)"
    $quick = Invoke-Ipbrs recover --source $src --backup $bk --dry-run --trust-mtime --threads $Threads
    $full = Invoke-Ipbrs recover --source $src --backup $bk --dry-run --threads $Threads
    Show "quick check (size + timestamp)" ("finds {0} damaged files in {1}" -f $quick.planned_files, (Sec $quick.total_ns))
    Show "full hash check (IPBRS)" ("finds {0} damaged files in {1}" -f $full.planned_files, (Sec $full.total_ns))
    Show "  missing / size changed / content" ("{0} / {1} / {2}" -f $full.missing, $full.size_changed, $full.content_damaged)
    if ($full.planned_files -gt $quick.planned_files) {
        Good ("hashing caught {0} silently corrupted files that timestamps miss" -f ($full.planned_files - $quick.planned_files))
    }

    Show-Step "selective block-level recovery"
    $r = Invoke-Ipbrs recover --source $src --backup $bk --threads $Threads
    Show "files restored" $r.restored_files
    Show "data written back" ("{0} of {1} ({2:N2}%)" -f (MiB $r.restored_bytes), (MiB $r.catalog_bytes), (100 * $r.restored_bytes / [math]::Max(1, $r.catalog_bytes)))
    Show "time" (Sec $r.total_ns)
    if ($r.failed_files -eq 0 -and $r.verify_bad -eq 0) { Good "every repaired file re-verified" } else { Bad "recovery" }

    Show-Step "the novelty: file-level vs IPBRS scheduling (verify the backup)"
    $seq = Invoke-Ipbrs verify --backup $bk --sched seq
    $fileLevel = Invoke-Ipbrs verify --backup $bk --threads $Threads --split none
    $ipbrs = Invoke-Ipbrs verify --backup $bk --threads $Threads --split adaptive
    Write-Host ("   {0,-24} {1,9} {2,9} {3,11} {4,8}" -f "", "time", "speedup", "imbalance", "idle")
    foreach ($row in @(@("sequential (1 thread)", $seq), @("file-level ($Threads threads)", $fileLevel), @("IPBRS ($Threads threads)", $ipbrs))) {
        $s = $row[1].scan.scheduler
        Write-Host ("   {0,-24} {1,9} {2,8:N1}x {3,11:N2} {4,7:N0}%" -f $row[0], (Sec $row[1].total_ns),
            ($seq.total_ns / $row[1].total_ns), $s.load_imbalance, (100 * $s.idle_fraction))
    }
    Good ("IPBRS is {0:N1}x faster than file-level scheduling on this dataset" -f ($fileLevel.total_ns / $ipbrs.total_ns))

    Show-Step "independent proof (Windows Get-FileHash, not our engine)"
    $after = Get-TreeHashes $src
    $mismatch = @($before.Keys | Where-Object { $after[$_] -ne $before[$_] })
    if ($after.Count -eq $before.Count -and $mismatch.Count -eq 0) {
        Good "all $($before.Count) files are byte-for-byte identical to the original"
    } else {
        Bad "$($mismatch.Count) files differ, $($after.Count) of $($before.Count) present"
    }
}
finally {
    if ($Keep) { Write-Host "`nkept demo files in $work" }
    elseif (Test-Path $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}

Write-Host ""
if ($failures -eq 0) { Write-Host "Demo finished: everything verified." -ForegroundColor Green }
else { Write-Host "Demo finished with $failures failed checks." -ForegroundColor Red; exit 1 }
