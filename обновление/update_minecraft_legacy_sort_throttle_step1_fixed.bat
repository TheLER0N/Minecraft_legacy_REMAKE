@echo off
chcp 65001 >nul
setlocal

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
"$ErrorActionPreference='Stop'; ^
$bat='%~f0'; ^
$updateDir=Split-Path -Parent $bat; ^
$root=(Resolve-Path (Join-Path $updateDir '..')).Path; ^
$raw=[IO.File]::ReadAllText($bat,[Text.UTF8Encoding]::new($false)); ^
$mark='# POWERSHELL_CODE'; ^
$idx=$raw.LastIndexOf($mark); ^
if($idx -lt 0){throw 'PowerShell marker not found'}; ^
$code=$raw.Substring($idx + $mark.Length); ^
Set-Variable -Name ProjectRoot -Scope Global -Value $root; ^
Set-Variable -Name UpdateDir -Scope Global -Value $updateDir; ^
Invoke-Expression $code"

set "ERR=%ERRORLEVEL%"

if "%ERR%"=="0" (
    echo.
    echo Patch finished successfully.
) else (
    echo.
    echo Patch failed. Error code: %ERR%
)

pause
exit /b %ERR%

# POWERSHELL_CODE

$ErrorActionPreference = 'Stop'

$BackupDir = Join-Path $UpdateDir ("backup_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
$ReportPath = Join-Path $UpdateDir "update_report.txt"

New-Item -ItemType Directory -Force -Path $BackupDir | Out-Null

function Get-RelativePathCompat([string]$BasePath, [string]$TargetPath) {
    $baseFull = [IO.Path]::GetFullPath($BasePath)
    if (-not $baseFull.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $baseFull += [IO.Path]::DirectorySeparatorChar
    }

    $targetFull = [IO.Path]::GetFullPath($TargetPath)
    $baseUri = New-Object System.Uri($baseFull)
    $targetUri = New-Object System.Uri($targetFull)
    $relativeUri = $baseUri.MakeRelativeUri($targetUri)
    $relativePath = [System.Uri]::UnescapeDataString($relativeUri.ToString())
    return $relativePath.Replace('/', [IO.Path]::DirectorySeparatorChar)
}

function Read-Utf8Text([string]$Path) {
    return [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false))
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "Write-Utf8NoBom received an empty path"
    }

    $dir = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }

    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Backup-File([string]$Path) {
    if (Test-Path $Path) {
        $relative = Get-RelativePathCompat $ProjectRoot $Path
        $target = Join-Path $BackupDir $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -Force $Path $target
    }
}

function Add-Log([System.Collections.Generic.List[string]]$Log, [string]$Text) {
    $Log.Add($Text) | Out-Null
}

function Find-MatchingParen([string]$Text, [int]$OpenIndex) {
    if ($OpenIndex -lt 0 -or $OpenIndex -ge $Text.Length -or $Text[$OpenIndex] -ne '(') {
        throw "Find-MatchingParen received invalid open index"
    }

    $depth = 0
    for ($i = $OpenIndex; $i -lt $Text.Length; ++$i) {
        $ch = $Text[$i]
        if ($ch -eq '(') {
            ++$depth
        } elseif ($ch -eq ')') {
            --$depth
            if ($depth -eq 0) {
                return $i
            }
        }
    }

    throw "Matching parenthesis not found"
}

function Find-MatchingBrace([string]$Text, [int]$OpenIndex) {
    if ($OpenIndex -lt 0 -or $OpenIndex -ge $Text.Length -or $Text[$OpenIndex] -ne '{') {
        throw "Find-MatchingBrace received invalid open index"
    }

    $depth = 0
    for ($i = $OpenIndex; $i -lt $Text.Length; ++$i) {
        $ch = $Text[$i]
        if ($ch -eq '{') {
            ++$depth
        } elseif ($ch -eq '}') {
            --$depth
            if ($depth -eq 0) {
                return $i
            }
        }
    }

    throw "Matching brace not found"
}

function Get-LineStart([string]$Text, [int]$Index) {
    $lineStart = $Text.LastIndexOf("`n", [Math]::Max(0, $Index))
    if ($lineStart -lt 0) {
        return 0
    }
    return $lineStart + 1
}

function Get-LineIndent([string]$Text, [int]$LineStart) {
    $i = $LineStart
    while ($i -lt $Text.Length -and ($Text[$i] -eq ' ' -or $Text[$i] -eq "`t")) {
        ++$i
    }
    return $Text.Substring($LineStart, $i - $LineStart)
}

function Wrap-SortCalls(
    [string]$Text,
    [string]$CallPrefix,
    [string]$NeedleInsideCall,
    [string]$LimitExpression,
    [string]$GuardMarker,
    [string]$Name,
    [System.Collections.Generic.List[string]]$Log
) {
    $index = 0
    $changed = 0

    while ($true) {
        $callIndex = $Text.IndexOf($CallPrefix, $index)
        if ($callIndex -lt 0) {
            break
        }

        $open = $Text.IndexOf("(", $callIndex)
        if ($open -lt 0) {
            break
        }

        $close = Find-MatchingParen $Text $open
        $semi = $Text.IndexOf(";", $close)
        if ($semi -lt 0) {
            break
        }

        $lineStart = Get-LineStart $Text $callIndex
        $end = $semi + 1
        while ($end -lt $Text.Length -and ($Text[$end] -eq "`r" -or $Text[$end] -eq "`n")) {
            ++$end
        }

        $statement = $Text.Substring($lineStart, $end - $lineStart)
        if (-not $statement.Contains($NeedleInsideCall)) {
            $index = $end
            continue
        }

        $lookBehindStart = [Math]::Max(0, $lineStart - 260)
        $lookBehind = $Text.Substring($lookBehindStart, $lineStart - $lookBehindStart)
        if ($lookBehind.Contains($GuardMarker)) {
            $index = $end
            continue
        }

        $indent = Get-LineIndent $Text $lineStart
        $guarded = $indent + "// " + $GuardMarker + "`r`n" +
            $indent + "if (" + $LimitExpression + ") {`r`n" +
            $statement +
            $indent + "}`r`n"

        $Text = $Text.Substring(0, $lineStart) + $guarded + $Text.Substring($end)
        $index = $lineStart + $guarded.Length
        ++$changed
    }

    Add-Log $Log ("OK: " + $Name + " guarded sort calls: " + $changed)
    return $Text
}

function Add-Guard-To-RebuildPriorityFunction(
    [string]$Text,
    [System.Collections.Generic.List[string]]$Log
) {
    $guardMarker = "ML_PERF_THROTTLE_JOB_PRIORITY_REBUILD"
    if ($Text.Contains($guardMarker)) {
        Add-Log $Log "SKIP: rebuild_job_queue_priority_locked guard already exists"
        return $Text
    }

    $nameIndex = $Text.IndexOf("WorldStreamer::rebuild_job_queue_priority_locked")
    if ($nameIndex -lt 0) {
        Add-Log $Log "WARN: rebuild_job_queue_priority_locked implementation not found in world_streamer.cpp; skipped guard"
        return $Text
    }

    $open = $Text.IndexOf("{", $nameIndex)
    if ($open -lt 0) {
        throw "Cannot guard rebuild_job_queue_priority_locked: opening brace not found"
    }

    $close = Find-MatchingBrace $Text $open
    if ($close -le $open) {
        throw "Cannot guard rebuild_job_queue_priority_locked: malformed function body"
    }

    $insert = $open + 1
    while ($insert -lt $Text.Length -and ($Text[$insert] -eq "`r" -or $Text[$insert] -eq "`n")) {
        ++$insert
    }

    $guard = @'

    // ML_PERF_THROTTLE_JOB_PRIORITY_REBUILD
    if (job_queue_.size() > kMaxJobQueuePriorityRebuildSize) {
        job_queue_priority_dirty_ = true;
        return;
    }

'@

    Add-Log $Log "OK: inserted flexible guard into rebuild_job_queue_priority_locked"
    return $Text.Substring(0, $insert) + $guard + $Text.Substring($insert)
}

$Log = New-Object System.Collections.Generic.List[string]
Add-Log $Log "Patch report"
Add-Log $Log ("Project root: " + $ProjectRoot)
Add-Log $Log ("Backup dir: " + $BackupDir)
Add-Log $Log ("Time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Log $Log ""

$WorldStreamerPath = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$ApplicationPath = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @($WorldStreamerPath, $ApplicationPath)

foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

foreach ($file in $RequiredFiles) {
    Backup-File $file
}

# ----------------------------------------------------------------------
# 1. world_streamer.cpp: add lightweight throttle constants.
# ----------------------------------------------------------------------

$world = Read-Utf8Text $WorldStreamerPath
$worldOriginal = $world

if (-not $world.Contains("kMaxExactStreamingBacklogSortSize")) {
    $marker = "constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;"
    if (-not $world.Contains($marker)) {
        throw "Cannot add throttle constants: kMaxDirtyChunkSavesPerTick marker not found"
    }

    $constants = @'
constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;
constexpr std::size_t kMaxExactStreamingBacklogSortSize = 768;
constexpr std::size_t kMaxExactJobQueueSortSize = 192;
constexpr std::size_t kMaxJobQueuePriorityRebuildSize = 384;
'@
    $world = $world.Replace($marker, $constants)
    Add-Log $Log "OK: added sort throttle constants"
} else {
    Add-Log $Log "SKIP: sort throttle constants already exist"
}

# ----------------------------------------------------------------------
# 2. Guard old exact sorts if they still exist.
# ----------------------------------------------------------------------

$world = Wrap-SortCalls `
    $world `
    "std::sort" `
    "streaming_backlog_.begin" `
    "streaming_backlog_.size() <= kMaxExactStreamingBacklogSortSize" `
    "ML_PERF_THROTTLE_STREAMING_BACKLOG_SORT" `
    "streaming_backlog_ std::sort" `
    $Log

$world = Wrap-SortCalls `
    $world `
    "std::stable_sort" `
    "job_queue_.begin" `
    "job_queue_.size() <= kMaxExactJobQueueSortSize" `
    "ML_PERF_THROTTLE_JOB_QUEUE_SORT" `
    "job_queue_ stable_sort" `
    $Log

# ----------------------------------------------------------------------
# 3. Guard new priority rebuild if implementation exists.
#    Previous version failed here because it required an exact one-line signature.
# ----------------------------------------------------------------------

$world = Add-Guard-To-RebuildPriorityFunction $world $Log

# ----------------------------------------------------------------------
# 4. Make sure constant draw-stat logging is off.
# ----------------------------------------------------------------------

$app = Read-Utf8Text $ApplicationPath
$appOriginal = $app

if ($app.Contains("renderer_.debug_log_draw_stats = true;")) {
    $app = $app.Replace("renderer_.debug_log_draw_stats = true;", "renderer_.debug_log_draw_stats = false;")
    Add-Log $Log "OK: disabled constant renderer draw-stat logging"
} else {
    Add-Log $Log "SKIP: renderer draw-stat logging already disabled or customized"
}

# ----------------------------------------------------------------------
# 5. Validation.
# ----------------------------------------------------------------------

if (-not $world.Contains("kMaxExactStreamingBacklogSortSize")) {
    throw "Validation failed: kMaxExactStreamingBacklogSortSize missing"
}

if (-not $world.Contains("kMaxExactJobQueueSortSize")) {
    throw "Validation failed: kMaxExactJobQueueSortSize missing"
}

if (-not $world.Contains("kMaxJobQueuePriorityRebuildSize")) {
    throw "Validation failed: kMaxJobQueuePriorityRebuildSize missing"
}

if ($world.Contains("std::sort(streaming_backlog_.begin") -and
    -not $world.Contains("ML_PERF_THROTTLE_STREAMING_BACKLOG_SORT")) {
    throw "Validation failed: unguarded streaming_backlog_ sort detected"
}

if ($world.Contains("std::stable_sort(job_queue_.begin") -and
    -not $world.Contains("ML_PERF_THROTTLE_JOB_QUEUE_SORT")) {
    throw "Validation failed: unguarded job_queue_ stable_sort detected"
}

# Important: only require the guard if the implementation is actually present in this .cpp.
if ($world.Contains("WorldStreamer::rebuild_job_queue_priority_locked") -and
    -not $world.Contains("ML_PERF_THROTTLE_JOB_PRIORITY_REBUILD")) {
    throw "Validation failed: rebuild_job_queue_priority_locked implementation exists but guard is missing"
}

if (-not $world.Contains("return load_area_chunk(origin, coord, chunk_radius);")) {
    throw "Validation failed: corridor_candidate_chunk is no longer priority-only compatibility helper"
}

if (-not $world.Contains("constexpr int kMaxChunkRadius = 24;")) {
    throw "Validation failed: desktop kMaxChunkRadius cap changed"
}

if ($world -ne $worldOriginal) {
    Write-Utf8NoBom $WorldStreamerPath $world
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
} else {
    Add-Log $Log "No world_streamer.cpp changes were needed."
}

if ($app -ne $appOriginal) {
    Write-Utf8NoBom $ApplicationPath $app
    Add-Log $Log "OK: src/app/application.cpp written"
} else {
    Add-Log $Log "No application.cpp changes were needed."
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Added sort throttling constants."
Add-Log $Log "- Guards old streaming_backlog_ sort if it still exists."
Add-Log $Log "- Guards old job_queue_ stable_sort if it still exists."
Add-Log $Log "- Guards rebuild_job_queue_priority_locked with flexible multiline signature detection."
Add-Log $Log "- Does not fail if rebuild_job_queue_priority_locked implementation is not present in the .cpp."
Add-Log $Log "- Keeps graphics, textures, shaders, lighting, render distance, and mesh generation unchanged."
Add-Log $Log "- Keeps corridor as priority-only logic."
Add-Log $Log ""
Add-Log $Log "Expected result:"
Add-Log $Log "- Fewer CPU spikes during world generation."
Add-Log $Log "- Less stutter from large queue sorting/reprioritization."
Add-Log $Log "- No visual changes."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build.bat."
Add-Log $Log "2. Test standing, walking, and fast flight."
Add-Log $Log "3. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
