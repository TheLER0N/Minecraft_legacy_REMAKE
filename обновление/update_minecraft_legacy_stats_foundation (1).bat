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

function Replace-BetweenMarkers(
    [string]$Text,
    [string]$StartMarker,
    [string]$EndMarker,
    [string]$Replacement,
    [string]$Name,
    [ref]$Changed,
    [System.Collections.Generic.List[string]]$Log
) {
    $start = $Text.IndexOf($StartMarker)
    if ($start -lt 0) {
        throw "Cannot replace $Name`: start marker not found"
    }

    $end = $Text.IndexOf($EndMarker, $start + $StartMarker.Length)
    if ($end -lt 0) {
        throw "Cannot replace $Name`: end marker not found"
    }

    $Changed.Value = $true
    Add-Log $Log ("OK: replaced " + $Name)
    return $Text.Substring(0, $start) + $Replacement + $Text.Substring($end)
}

$Log = New-Object System.Collections.Generic.List[string]
Add-Log $Log "Patch report"
Add-Log $Log ("Project root: " + $ProjectRoot)
Add-Log $Log ("Backup dir: " + $BackupDir)
Add-Log $Log ("Time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Log $Log ""

$WorldStreamerHpp = Join-Path $ProjectRoot "src\game\world_streamer.hpp"
$WorldStreamerCpp = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$RendererHpp = Join-Path $ProjectRoot "src\render\renderer.hpp"
$RendererCpp = Join-Path $ProjectRoot "src\render\renderer.cpp"
$ApplicationCpp = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @(
    $WorldStreamerHpp,
    $WorldStreamerCpp,
    $RendererHpp,
    $RendererCpp,
    $ApplicationCpp
)

foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

foreach ($file in $RequiredFiles) {
    Backup-File $file
}

# ----------------------------------------------------------------------
# 1. WorldStreamer::StreamingStats contract.
#    This aligns runtime stats with existing debug/perf recorder fields.
# ----------------------------------------------------------------------

$h = Read-Utf8Text $WorldStreamerHpp
$hChanged = $false

$StreamingStatsBlock = @'
    struct StreamingStats {
        std::size_t visible_chunks {0};
        std::size_t loaded_chunks {0};
        std::size_t pending_uploads {0};
        std::size_t pending_upload_bytes {0};
        std::size_t pending_upload_sections {0};
        std::size_t pending_unloads {0};
        std::size_t queued_rebuilds {0};
        std::size_t queued_generates {0};
        std::size_t queued_decorates {0};
        std::size_t queued_lights {0};
        std::size_t queued_meshes {0};
        std::size_t queued_fast_meshes {0};
        std::size_t queued_final_meshes {0};
        std::size_t completed_results {0};
        std::size_t streaming_backlog_size {0};
        std::size_t streaming_backlog_remaining {0};
        std::size_t stale_results {0};
        std::size_t stale_uploads {0};
        std::size_t provisional_uploads {0};
        std::size_t provisional_lifetime_frames {0};
        std::size_t light_stale_results {0};
        std::size_t edge_fixups {0};
        std::size_t dropped_jobs {0};
        std::size_t dirty_save_chunks {0};
        std::size_t missing_light_borders {0};
        std::size_t urgent_edit_chunks {0};
        std::size_t urgent_edit_uploads {0};
        std::size_t edit_upload_latency_frames {0};
        std::size_t renderer_upload_failures {0};
        bool observer_light_borders_ready {false};
        int observer_light_border_status {0};
        float last_generate_ms {0.0f};
        float last_light_ms {0.0f};
        float last_mesh_ms {0.0f};
        float last_apply_ms {0.0f};
    };
'@

$h = Replace-BetweenMarkers `
    $h `
    "    struct StreamingStats {" `
    "    WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius = 6);" `
    ($StreamingStatsBlock + "`r`n`r`n") `
    "WorldStreamer::StreamingStats expanded stats contract" `
    ([ref]$hChanged) `
    $Log

if ($hChanged) {
    Write-Utf8NoBom $WorldStreamerHpp $h
    Add-Log $Log "OK: src/game/world_streamer.hpp written"
}

# ----------------------------------------------------------------------
# 2. WorldStreamer::stats implementation.
# ----------------------------------------------------------------------

$cpp = Read-Utf8Text $WorldStreamerCpp
$cppChanged = $false

$StatsMethod = @'
WorldStreamer::StreamingStats WorldStreamer::stats() const {
    StreamingStats stats {};

    std::lock_guard lock(mutex_);

    stats.visible_chunks = visible_chunks_.size();
    stats.loaded_chunks = chunks_.size();
    stats.pending_uploads = pending_uploads_.size();
    stats.pending_unloads = pending_unloads_.size();
    stats.completed_results = completed_.size();
    stats.streaming_backlog_size = streaming_backlog_.size();
    stats.streaming_backlog_remaining =
        streaming_backlog_cursor_ < streaming_backlog_.size()
            ? streaming_backlog_.size() - streaming_backlog_cursor_
            : 0;

    for (const PendingChunkUpload& upload : pending_uploads_) {
        stats.pending_upload_bytes += mesh_byte_count(upload.mesh);
        if (mesh_vertex_count(upload.mesh) > 0 || mesh_index_count(upload.mesh) > 0) {
            ++stats.pending_upload_sections;
        }
        if (upload.provisional) {
            ++stats.provisional_uploads;
        }
    }

    for (const ChunkJob& job : job_queue_) {
        switch (job.type) {
        case ChunkJobType::GenerateTerrain:
            ++stats.queued_generates;
            break;
        case ChunkJobType::Decorate:
            ++stats.queued_decorates;
            break;
        case ChunkJobType::CalculateLight:
            ++stats.queued_lights;
            break;
        case ChunkJobType::BuildMesh:
            ++stats.queued_meshes;
            if (job.snapshot != nullptr && job.snapshot->provisional) {
                ++stats.queued_fast_meshes;
            } else {
                ++stats.queued_final_meshes;
            }
            break;
        }
    }

    for (const auto& [coord, rebuild] : rebuild_states_) {
        (void)coord;
        if (rebuild.queued || rebuild.dirty) {
            ++stats.queued_rebuilds;
        }
    }

    stats.stale_results = stale_results_;
    stats.stale_uploads = stale_uploads_dropped_;
    stats.light_stale_results = light_stale_results_;
    stats.edge_fixups = edge_fixups_;
    stats.dropped_jobs = dropped_jobs_;
    stats.dirty_save_chunks = dirty_save_set_.size();
    stats.last_generate_ms = last_generate_ms_;
    stats.last_light_ms = last_light_ms_;
    stats.last_mesh_ms = last_mesh_ms_;
    stats.last_apply_ms = 0.0f;

    stats.observer_light_borders_ready = true;
    stats.observer_light_border_status = 0;

    return stats;
}

'@

if ($cpp.Contains("WorldStreamer::StreamingStats WorldStreamer::stats() const {")) {
    $cpp = Replace-BetweenMarkers `
        $cpp `
        "WorldStreamer::StreamingStats WorldStreamer::stats() const {" `
        "BlockQueryResult WorldStreamer::query_block_at_world" `
        ($StatsMethod + "BlockQueryResult WorldStreamer::query_block_at_world") `
        "WorldStreamer::stats comprehensive counters" `
        ([ref]$cppChanged) `
        $Log
} else {
    throw "Cannot patch world_streamer.cpp: WorldStreamer::stats() marker not found"
}

if ($cppChanged) {
    Write-Utf8NoBom $WorldStreamerCpp $cpp
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
}

# ----------------------------------------------------------------------
# 3. Renderer stats accessors for future perf recorder / debug.
# ----------------------------------------------------------------------

$rh = Read-Utf8Text $RendererHpp
$rhChanged = $false

if (-not $rh.Contains("struct BufferStats")) {
    $bufferStatsBlock = @'

    struct BufferStats {
        std::size_t resident_chunk_meshes {0};
        std::size_t pooled_gpu_buffers {0};
        std::size_t deferred_chunk_meshes {0};
        std::size_t new_gpu_buffers {0};
    };
'@

    $rh = $rh.Replace("    ~Renderer();", $bufferStatsBlock + "`r`n    ~Renderer();")
    $rhChanged = $true
    Add-Log $Log "OK: renderer.hpp added BufferStats"
}

if (-not $rh.Contains("DebugHudData last_draw_stats() const;")) {
    $anchor = "    std::size_t resident_chunk_mesh_count() const;"
    if (-not $rh.Contains($anchor)) {
        throw "Cannot patch renderer.hpp: resident_chunk_mesh_count declaration anchor not found"
    }

    $rh = $rh.Replace(
        $anchor,
        $anchor + "`r`n    DebugHudData last_draw_stats() const;`r`n    BufferStats buffer_stats() const;"
    )
    $rhChanged = $true
    Add-Log $Log "OK: renderer.hpp added stats accessors"
}

if ($rhChanged) {
    Write-Utf8NoBom $RendererHpp $rh
    Add-Log $Log "OK: src/render/renderer.hpp written"
}

$rcpp = Read-Utf8Text $RendererCpp
$rcppChanged = $false

if (-not $rcpp.Contains("Renderer::DebugHudData Renderer::last_draw_stats() const")) {
    $accessors = @'

Renderer::DebugHudData Renderer::last_draw_stats() const {
    return debug_hud_data_;
}

Renderer::BufferStats Renderer::buffer_stats() const {
    BufferStats stats {};
    stats.resident_chunk_meshes = chunk_buffers_.size();
    stats.pooled_gpu_buffers = chunk_buffer_pool_.size();
    stats.deferred_chunk_meshes = deferred_chunk_buffers_.size();
    stats.new_gpu_buffers = chunk_buffers_.size();
    return stats;
}

'@

    $insertMarker = "void Renderer::set_cave_visibility_frame"
    if (-not $rcpp.Contains($insertMarker)) {
        $insertMarker = "void Renderer::draw_visible_chunks"
    }
    if (-not $rcpp.Contains($insertMarker)) {
        throw "Cannot patch renderer.cpp: stats accessor insertion marker not found"
    }

    $idx = $rcpp.IndexOf($insertMarker)
    $rcpp = $rcpp.Substring(0, $idx) + $accessors + $rcpp.Substring($idx)
    $rcppChanged = $true
    Add-Log $Log "OK: renderer.cpp inserted stats accessors"
}

if ($rcppChanged) {
    Write-Utf8NoBom $RendererCpp $rcpp
    Add-Log $Log "OK: src/render/renderer.cpp written"
}

# ----------------------------------------------------------------------
# 4. Enable existing draw stats logging.
# ----------------------------------------------------------------------

$app = Read-Utf8Text $ApplicationCpp
$appChanged = $false

if ($app.Contains("renderer_.debug_log_draw_stats = false;")) {
    $app = $app.Replace(
        "renderer_.debug_log_draw_stats = false;",
        "renderer_.debug_log_draw_stats = true;"
    )
    $appChanged = $true
    Add-Log $Log "OK: application.cpp enabled renderer draw stats logging"
}

if ($appChanged) {
    Write-Utf8NoBom $ApplicationCpp $app
    Add-Log $Log "OK: src/app/application.cpp written"
}

# ----------------------------------------------------------------------
# 5. Validation.
# ----------------------------------------------------------------------

$hAfter = Read-Utf8Text $WorldStreamerHpp
$cppAfter = Read-Utf8Text $WorldStreamerCpp
$rhAfter = Read-Utf8Text $RendererHpp
$rcppAfter = Read-Utf8Text $RendererCpp
$appAfter = Read-Utf8Text $ApplicationCpp

if (-not $hAfter.Contains("std::size_t loaded_chunks {0};")) {
    throw "Validation failed: loaded_chunks missing in StreamingStats"
}

if (-not $hAfter.Contains("float last_apply_ms {0.0f};")) {
    throw "Validation failed: last_apply_ms missing in StreamingStats"
}

if (-not $cppAfter.Contains("stats.loaded_chunks = chunks_.size();")) {
    throw "Validation failed: WorldStreamer::stats does not count loaded chunks"
}

if (-not $cppAfter.Contains("stats.pending_upload_bytes += mesh_byte_count(upload.mesh);")) {
    throw "Validation failed: WorldStreamer::stats does not count upload bytes"
}

if (-not $rhAfter.Contains("struct BufferStats")) {
    throw "Validation failed: Renderer::BufferStats missing"
}

if (-not $rhAfter.Contains("DebugHudData last_draw_stats() const;")) {
    throw "Validation failed: Renderer::last_draw_stats declaration missing"
}

if (-not $rcppAfter.Contains("Renderer::DebugHudData Renderer::last_draw_stats() const")) {
    throw "Validation failed: Renderer::last_draw_stats implementation missing"
}

if (-not $rcppAfter.Contains("Renderer::BufferStats Renderer::buffer_stats() const")) {
    throw "Validation failed: Renderer::buffer_stats implementation missing"
}

if (-not $appAfter.Contains("renderer_.debug_log_draw_stats = true;")) {
    Add-Log $Log "WARN: renderer debug draw stats assignment not found or already customized"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_streamer.hpp"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log "- src/render/renderer.hpp"
Add-Log $Log "- src/render/renderer.cpp"
if ($appChanged) {
    Add-Log $Log "- src/app/application.cpp"
}
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Expanded WorldStreamer stats so optimization can be measured instead of guessed."
Add-Log $Log "- WorldStreamer::stats now counts loaded/visible chunks, job queues, pending uploads, bytes, dirty saves, stale results, and timing."
Add-Log $Log "- Added Renderer stats accessors for future frame/perf recording."
Add-Log $Log "- Enabled existing renderer draw-stat logging if the project has that path implemented."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build.bat or build_release.bat."
Add-Log $Log "2. Test normal walking and fast flight."
Add-Log $Log "3. Check logs for draw/chunk stats."
Add-Log $Log "4. Send the first compiler error and 30 lines after it if build fails."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
