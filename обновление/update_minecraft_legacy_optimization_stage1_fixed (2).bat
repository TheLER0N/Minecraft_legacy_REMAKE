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

$Log = New-Object System.Collections.Generic.List[string]
Add-Log $Log "Patch report"
Add-Log $Log ("Project root: " + $ProjectRoot)
Add-Log $Log ("Backup dir: " + $BackupDir)
Add-Log $Log ("Time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Log $Log ""

$WorldStreamer = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$RuntimeTuning = Join-Path $ProjectRoot "src\game\world_runtime_tuning.hpp"

if (-not (Test-Path $WorldStreamer)) {
    throw "Required file not found: $WorldStreamer"
}

Backup-File $WorldStreamer
if (Test-Path $RuntimeTuning) {
    Backup-File $RuntimeTuning
}

# ----------------------------------------------------------------------
# 1. Runtime tuning header. Safe rewrite: stable target state.
# ----------------------------------------------------------------------

$runtimeTuningText = @'
#pragma once

#include <algorithm>
#include <cstddef>
#include <thread>

namespace ml {

struct WorldRuntimeTuning {
    std::size_t worker_count {2};
    std::size_t max_new_chunk_requests_per_frame {8};
    std::size_t max_completed_results_per_tick {48};
    std::size_t max_job_queue_size {512};
    std::size_t chunk_upload_byte_budget_per_frame {2ull * 1024ull * 1024ull};
    std::size_t chunk_upload_backlog_budget_per_frame {4ull * 1024ull * 1024ull};
    std::size_t chunk_upload_max_count_per_frame {2};
    float completed_result_apply_budget_ms {1.5f};
};

inline WorldRuntimeTuning world_runtime_tuning() {
    const std::size_t hardware_threads =
        std::max<std::size_t>(2, std::thread::hardware_concurrency());

#ifdef __ANDROID__
    const std::size_t worker_count = std::clamp<std::size_t>(
        hardware_threads > 1 ? hardware_threads - 1 : 1,
        std::size_t {1},
        std::size_t {2}
    );

    return WorldRuntimeTuning {
        worker_count,
        std::size_t {2},
        std::size_t {10},
        std::size_t {192},
        512ull * 1024ull,
        1024ull * 1024ull,
        std::size_t {2},
        1.0f
    };
#else
    const std::size_t reserved_threads = hardware_threads >= 8 ? 2 : 1;
    const std::size_t available_workers =
        hardware_threads > reserved_threads ? hardware_threads - reserved_threads : 2;

    const std::size_t worker_count = std::clamp<std::size_t>(
        available_workers,
        std::size_t {2},
        std::size_t {12}
    );

    return WorldRuntimeTuning {
        worker_count,
        std::clamp<std::size_t>(worker_count * 3, std::size_t {8}, std::size_t {24}),
        std::clamp<std::size_t>(worker_count * 12, std::size_t {48}, std::size_t {144}),
        std::clamp<std::size_t>(worker_count * 96, std::size_t {384}, std::size_t {1536}),
        3ull * 1024ull * 1024ull,
        6ull * 1024ull * 1024ull,
        std::clamp<std::size_t>(worker_count / 2, std::size_t {2}, std::size_t {6}),
        1.5f
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuning $runtimeTuningText
Add-Log $Log "OK: src/game/world_runtime_tuning.hpp written to stable target state"

# ----------------------------------------------------------------------
# 2. world_streamer.cpp repair and optimization stage 1.
# ----------------------------------------------------------------------

$ws = Read-Utf8Text $WorldStreamer
$wsChanged = $false

if (-not $ws.Contains('#include "game/world_runtime_tuning.hpp"')) {
    if ($ws.Contains('#include "game/world_streamer.hpp"')) {
        $ws = $ws.Replace(
            '#include "game/world_streamer.hpp"',
            "#include `"game/world_streamer.hpp`"`r`n`r`n#include `"game/world_runtime_tuning.hpp`""
        )
        $wsChanged = $true
        Add-Log $Log "OK: added runtime tuning include"
    } else {
        throw "Cannot add runtime tuning include: base include was not found"
    }
} else {
    Add-Log $Log "OK: runtime tuning include already exists"
}

if ($ws.Contains("}constexpr std::size_t kMaxDirtyChunkSavesPerTick")) {
    $ws = $ws.Replace(
        "}constexpr std::size_t kMaxDirtyChunkSavesPerTick",
        "}`r`n`r`nconstexpr std::size_t kMaxDirtyChunkSavesPerTick"
    )
    $wsChanged = $true
    Add-Log $Log "OK: fixed '}constexpr' formatting defect"
} else {
    Add-Log $Log "OK: no '}constexpr' defect found"
}

# Normalize helper block if the old helper sequence exists.
$helperPattern = '(?s)std::size_t\s+max_new_chunk_requests_per_frame\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.max_new_chunk_requests_per_frame;\s*\}\s*std::size_t\s+max_completed_results_per_tick\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.max_completed_results_per_tick;\s*\}\s*std::size_t\s+max_job_queue_size\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.max_job_queue_size;\s*\}(?:\s*float\s+completed_result_apply_budget_ms\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.completed_result_apply_budget_ms;\s*\})?'
$helperBlock = @'
std::size_t max_new_chunk_requests_per_frame() {
    return world_runtime_tuning().max_new_chunk_requests_per_frame;
}

std::size_t max_completed_results_per_tick() {
    return world_runtime_tuning().max_completed_results_per_tick;
}

std::size_t max_job_queue_size() {
    return world_runtime_tuning().max_job_queue_size;
}

float completed_result_apply_budget_ms() {
    return world_runtime_tuning().completed_result_apply_budget_ms;
}
'@

if ([regex]::IsMatch($ws, $helperPattern)) {
    $ws = [regex]::Replace(
        $ws,
        $helperPattern,
        [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $helperBlock },
        1
    )
    $wsChanged = $true
    Add-Log $Log "OK: normalized runtime helper block"
} elseif (-not $ws.Contains("completed_result_apply_budget_ms()")) {
    $anchor = @'
std::size_t max_job_queue_size() {
    return world_runtime_tuning().max_job_queue_size;
}
'@
    $insert = @'
std::size_t max_job_queue_size() {
    return world_runtime_tuning().max_job_queue_size;
}

float completed_result_apply_budget_ms() {
    return world_runtime_tuning().completed_result_apply_budget_ms;
}
'@
    if ($ws.Contains($anchor)) {
        $ws = $ws.Replace($anchor, $insert)
        $wsChanged = $true
        Add-Log $Log "OK: added completed_result_apply_budget_ms helper"
    } else {
        throw "Cannot insert completed_result_apply_budget_ms helper: max_job_queue_size anchor not found"
    }
} else {
    Add-Log $Log "OK: completed_result_apply_budget_ms helper already exists"
}

# Robust backpressure insertion:
# Insert before the first "ChunkRecord record {};" in update_observer. This is intentionally line-anchored,
# not full-block anchored, because local whitespace may differ.
if (-not $ws.Contains("job_queue_.size() >= max_job_queue_size()")) {
    $recordPattern = '(?m)^(\s*)ChunkRecord\s+record\s*\{\};\s*$'
    $recordMatch = [regex]::Match($ws, $recordPattern)

    if (-not $recordMatch.Success) {
        throw "Cannot insert backpressure: 'ChunkRecord record {};' line not found"
    }

    $indent = $recordMatch.Groups[1].Value
    $guard = (
        $indent + "{`r`n" +
        $indent + "    std::lock_guard lock(mutex_);`r`n" +
        $indent + "    if (job_queue_.size() >= max_job_queue_size()) {`r`n" +
        $indent + "        requested_this_frame = max_new_chunk_requests_per_frame();`r`n" +
        $indent + "        break;`r`n" +
        $indent + "    }`r`n" +
        $indent + "}`r`n`r`n"
    )

    $replacement = $guard + $recordMatch.Value
    $ws = $ws.Substring(0, $recordMatch.Index) + $replacement + $ws.Substring($recordMatch.Index + $recordMatch.Length)
    $wsChanged = $true
    Add-Log $Log "OK: inserted job queue backpressure before new chunk record creation"
} else {
    Add-Log $Log "OK: job queue backpressure already exists"
}

# Add time-budget check to completed result processing if it is still missing.
if (-not $ws.Contains("const auto apply_start = std::chrono::steady_clock::now();")) {
    $tickPattern = '(?s)void\s+WorldStreamer::tick_generation_jobs\(\)\s*\{\s*std::lock_guard\s+lock\(mutex_\);\s*std::size_t\s+processed\s*=\s*0;\s*while\s*\(!completed_\.empty\(\)\s*&&\s*processed\s*<\s*max_completed_results_per_tick\(\)\)\s*\{\s*JobResult\s+result\s*=\s*std::move\(completed_\.front\(\)\);'
    $tickReplacement = @'
void WorldStreamer::tick_generation_jobs() {
    std::lock_guard lock(mutex_);
    std::size_t processed = 0;
    const auto apply_start = std::chrono::steady_clock::now();
    while (!completed_.empty() && processed < max_completed_results_per_tick()) {
        if (processed > 0) {
            const float apply_ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - apply_start
            ).count();

            if (apply_ms >= completed_result_apply_budget_ms()) {
                break;
            }
        }

        JobResult result = std::move(completed_.front());
'@

    if ([regex]::IsMatch($ws, $tickPattern)) {
        $ws = [regex]::Replace(
            $ws,
            $tickPattern,
            [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $tickReplacement },
            1
        )
        $wsChanged = $true
        Add-Log $Log "OK: inserted completed result apply time budget"
    } else {
        throw "Cannot insert completed result time budget: tick_generation_jobs pattern not found"
    }
} else {
    Add-Log $Log "OK: completed result apply time budget already exists"
}

if ($wsChanged) {
    Write-Utf8NoBom $WorldStreamer $ws
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
} else {
    Add-Log $Log "NO CHANGE: src/game/world_streamer.cpp already matched target state"
}

# ----------------------------------------------------------------------
# 3. Validation.
# ----------------------------------------------------------------------

$wsAfter = Read-Utf8Text $WorldStreamer

if ($wsAfter.Contains("}constexpr")) {
    throw "Validation failed: world_streamer.cpp still contains '}constexpr'"
}

if ($wsAfter.Contains("constexpr std::size_t max_completed_results_per_tick()")) {
    throw "Validation failed: broken constexpr function declaration remains"
}

if (-not $wsAfter.Contains("float completed_result_apply_budget_ms()")) {
    throw "Validation failed: completed_result_apply_budget_ms helper missing"
}

if (-not $wsAfter.Contains("const auto apply_start = std::chrono::steady_clock::now();")) {
    throw "Validation failed: completed-result time budget missing"
}

if (-not $wsAfter.Contains("job_queue_.size() >= max_job_queue_size()")) {
    throw "Validation failed: job queue backpressure guard missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Repaired stage1 partial patch state."
Add-Log $Log "- Fixed the '}constexpr' formatting defect."
Add-Log $Log "- Added completed_result_apply_budget_ms to runtime tuning."
Add-Log $Log "- Added time-budgeted processing in WorldStreamer::tick_generation_jobs()."
Add-Log $Log "- Added robust job_queue_ backpressure before new chunk requests."
Add-Log $Log "- Did not reduce graphics quality, render distance, lighting, water, leaves, or mesh detail."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Rebuild Debug."
Add-Log $Log "2. If Debug passes, rebuild Release."
Add-Log $Log "3. Test movement while generating new chunks."
Add-Log $Log "4. If compiler fails, send the first error line and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
