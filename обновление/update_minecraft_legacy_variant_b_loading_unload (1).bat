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

function Replace-BetweenMarkers([string]$Text, [string]$StartMarker, [string]$EndMarker, [string]$Replacement, [string]$Name, [ref]$Changed, [System.Collections.Generic.List[string]]$Log) {
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

$RuntimeTuningPath = Join-Path $ProjectRoot "src\game\world_runtime_tuning.hpp"
$RendererHppPath = Join-Path $ProjectRoot "src\render\renderer.hpp"
$RendererCppPath = Join-Path $ProjectRoot "src\render\renderer.cpp"
$ApplicationCppPath = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @($RuntimeTuningPath, $RendererHppPath, $RendererCppPath, $ApplicationCppPath)
foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $RuntimeTuningPath
Backup-File $RendererHppPath
Backup-File $RendererCppPath
Backup-File $ApplicationCppPath

# ----------------------------------------------------------------------
# 1. Runtime tuning: practical loading variant B.
#    Variant B = do not wait for all 1089 radius-16 chunks.
#    Wait for safe 5x5 spawn area, then let normal streaming finish the rest.
# ----------------------------------------------------------------------

$RuntimeTuningText = @'
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
    int target_chunk_radius {16};
    float streaming_update_distance_blocks {4.0f};
    float forward_priority_weight {30.0f};
    float side_priority_weight {18.0f};
    float back_priority_penalty {1000.0f};
    int min_forward_buffer_chunks {3};
    int max_forward_buffer_chunks {8};
    int min_forward_width_chunks {3};
    int max_forward_width_chunks {7};
    float forward_buffer_pipeline_seconds {0.75f};
    float forward_buffer_safety_blocks {24.0f};
    float fast_flight_speed_threshold {25.0f};
    float very_fast_flight_speed_threshold {45.0f};
    int spawn_preload_radius {2};
    std::size_t spawn_preload_min_visible_chunks {25};
    int spawn_preload_max_frames {1200};
    std::size_t spawn_preload_requests_per_frame {20};
    std::size_t spawn_preload_upload_max_count {20};
    std::size_t streaming_backlog_requests_per_frame {8};
    std::size_t world_exit_mesh_unload_budget_per_step {128};
    int transition_black_frames {30};
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
        1.0f,
        8,
        4.0f,
        24.0f,
        18.0f,
        1000.0f,
        3,
        5,
        3,
        5,
        0.75f,
        20.0f,
        18.0f,
        35.0f,
        1,
        std::size_t {9},
        700,
        std::size_t {8},
        std::size_t {8},
        std::size_t {4},
        std::size_t {64},
        30
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
        1.5f,
        16,
        4.0f,
        30.0f,
        18.0f,
        1000.0f,
        3,
        8,
        3,
        7,
        0.75f,
        24.0f,
        25.0f,
        45.0f,
        2,
        std::size_t {25},
        1200,
        std::size_t {20},
        std::size_t {20},
        std::size_t {8},
        std::size_t {128},
        30
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuningPath $RuntimeTuningText
Add-Log $Log "OK: world_runtime_tuning.hpp rewritten for variant B"
Add-Log $Log "    - desktop spawn_preload_radius = 2"
Add-Log $Log "    - desktop spawn_preload_min_visible_chunks = 25"
Add-Log $Log "    - rest of radius 16 continues via streaming backlog after entry"

# ----------------------------------------------------------------------
# 2. Renderer: unload every resident chunk mesh, not only visible chunks.
# ----------------------------------------------------------------------

$RendererHppText = Read-Utf8Text $RendererHppPath
$RendererHppChanged = $false

if (-not $RendererHppText.Contains("void unload_all_chunk_meshes();")) {
    $oldDecl = '    void unload_chunk_mesh(ChunkCoord coord);'
    $newDecl = @'
    void unload_chunk_mesh(ChunkCoord coord);
    void unload_all_chunk_meshes();
    std::size_t resident_chunk_mesh_count() const;
'@

    if ($RendererHppText.Contains($oldDecl)) {
        $RendererHppText = $RendererHppText.Replace($oldDecl, $newDecl.TrimEnd())
        $RendererHppChanged = $true
        Add-Log $Log "OK: renderer.hpp added unload_all_chunk_meshes and resident_chunk_mesh_count"
    } else {
        throw "Cannot patch renderer.hpp: unload_chunk_mesh declaration anchor not found"
    }
} else {
    Add-Log $Log "OK: renderer.hpp unload_all_chunk_meshes already exists"
}

if ($RendererHppChanged) {
    Write-Utf8NoBom $RendererHppPath $RendererHppText
    Add-Log $Log "OK: src/render/renderer.hpp written"
}

$RendererCppText = Read-Utf8Text $RendererCppPath
$RendererCppChanged = $false

$RendererUnloadAllMethod = @'
void Renderer::unload_all_chunk_meshes() {
    for (auto& [coord, render_data] : chunk_buffers_) {
        (void)coord;
        for (RenderSection& section : render_data.sections) {
            destroy_render_section(section);
        }
    }

    chunk_buffers_.clear();
    destroy_deferred_chunk_buffers_immediate();
}

std::size_t Renderer::resident_chunk_mesh_count() const {
    return chunk_buffers_.size();
}

'@

if (-not $RendererCppText.Contains("void Renderer::unload_all_chunk_meshes()")) {
    $insertMarker = "void Renderer::unload_chunk_mesh(ChunkCoord coord)"
    $insertAt = $RendererCppText.IndexOf($insertMarker)

    if ($insertAt -lt 0) {
        $insertMarker = "bool Renderer::upload_chunk_mesh"
        $insertAt = $RendererCppText.IndexOf($insertMarker)
    }

    if ($insertAt -lt 0) {
        throw "Cannot insert renderer unload_all_chunk_meshes: insertion marker not found"
    }

    $RendererCppText = $RendererCppText.Substring(0, $insertAt) + $RendererUnloadAllMethod + $RendererCppText.Substring($insertAt)
    $RendererCppChanged = $true
    Add-Log $Log "OK: renderer.cpp inserted unload_all_chunk_meshes implementation"
} else {
    Add-Log $Log "OK: renderer.cpp unload_all_chunk_meshes already exists"
}

if ($RendererCppChanged) {
    Write-Utf8NoBom $RendererCppPath $RendererCppText
    Add-Log $Log "OK: src/render/renderer.cpp written"
}

# ----------------------------------------------------------------------
# 3. Application: practical 5x5 spawn loading and complete world unload.
# ----------------------------------------------------------------------

$ApplicationCppText = Read-Utf8Text $ApplicationCppPath
$ApplicationCppChanged = $false

if (-not $ApplicationCppText.Contains("#include <vector>")) {
    if ($ApplicationCppText.Contains("#include <string>")) {
        $ApplicationCppText = $ApplicationCppText.Replace("#include <string>", "#include <string>`r`n#include <vector>")
        $ApplicationCppChanged = $true
        Add-Log $Log "OK: application.cpp added <vector> include"
    } else {
        Add-Log $Log "WARN: application.cpp <string> include anchor not found; <vector> not inserted"
    }
}

$PreloadWorldSpawnFunction = @'
bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {
    if (world_streamer_ == nullptr) {
        return false;
    }

    render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Загрузка мира");

    const int spawn_block_x = static_cast<int>(std::floor(spawn_position.x));
    const int spawn_block_z = static_cast<int>(std::floor(spawn_position.z));
    const int probe_y = std::clamp(
        static_cast<int>(std::floor(spawn_position.y)) - 1,
        kWorldMinY,
        kWorldMaxY
    );

    const int max_frames = world_runtime_tuning().spawn_preload_max_frames;
    const int preload_radius = world_runtime_tuning().spawn_preload_radius;
    const std::size_t required_area_chunks =
        static_cast<std::size_t>((preload_radius * 2 + 1) * (preload_radius * 2 + 1));
    const std::size_t min_visible_chunks =
        std::max(world_runtime_tuning().spawn_preload_min_visible_chunks, required_area_chunks);
    const std::size_t requests_per_frame = world_runtime_tuning().spawn_preload_requests_per_frame;
    const std::size_t upload_max_count = world_runtime_tuning().spawn_preload_upload_max_count;

    std::vector<std::array<int, 2>> preload_probe_offsets;
    preload_probe_offsets.reserve(required_area_chunks);
    for (int dz = -preload_radius; dz <= preload_radius; ++dz) {
        for (int dx = -preload_radius; dx <= preload_radius; ++dx) {
            preload_probe_offsets.push_back({{dx * kChunkWidth, dz * kChunkDepth}});
        }
    }

    bool spawn_column_loaded_once = false;

    log_message(
        LogLevel::Info,
        std::string("Application: variant B preload begin [radius=") +
            std::to_string(preload_radius) +
            ", required_area_chunks=" + std::to_string(required_area_chunks) +
            ", min_visible=" + std::to_string(min_visible_chunks) +
            ", max_frames=" + std::to_string(max_frames) + "]"
    );

    for (int frame = 0; frame < max_frames && !platform_.should_close(); ++frame) {
        platform_.pump_events();

        world_streamer_->request_spawn_preload(spawn_position, preload_radius, requests_per_frame);

        for (int apply_tick = 0; apply_tick < 4; ++apply_tick) {
            world_streamer_->tick_generation_jobs();
        }

        for (const ChunkCoord& coord : world_streamer_->drain_pending_unloads()) {
            renderer_.unload_chunk_mesh(coord);
        }

        auto pending_uploads = world_streamer_->drain_pending_uploads_by_budget(
            chunk_upload_backlog_budget_per_frame(),
            upload_max_count,
            spawn_position,
            spawn_forward
        );

        for (PendingChunkUpload& upload : pending_uploads) {
            const bool upload_ok = renderer_.upload_chunk_mesh(upload.coord, upload.mesh, upload.visibility);
            if (upload_ok) {
                world_streamer_->confirm_chunk_uploaded(
                    upload.coord,
                    upload.version,
                    upload.rebuild_serial,
                    upload.upload_token
                );
            }
        }

        world_streamer_->refresh_visible_chunks();

        int loaded_probe_columns = 0;
        for (const auto& offset : preload_probe_offsets) {
            const BlockQueryResult query = world_streamer_->query_block_at_world(
                spawn_block_x + offset[0],
                probe_y,
                spawn_block_z + offset[1]
            );

            if (query.status == BlockQueryStatus::Loaded) {
                ++loaded_probe_columns;
            }
        }

        const BlockQueryResult spawn_probe = world_streamer_->query_block_at_world(
            spawn_block_x,
            probe_y,
            spawn_block_z
        );

        spawn_column_loaded_once = spawn_column_loaded_once || spawn_probe.status == BlockQueryStatus::Loaded;
        const WorldStreamer::StreamingStats stats = world_streamer_->stats();

        if (spawn_column_loaded_once &&
            loaded_probe_columns >= static_cast<int>(preload_probe_offsets.size()) &&
            stats.visible_chunks >= min_visible_chunks) {
            log_message(
                LogLevel::Info,
                std::string("Application: variant B preload done [frame=") +
                    std::to_string(frame) +
                    ", visible=" + std::to_string(stats.visible_chunks) +
                    ", probes=" + std::to_string(loaded_probe_columns) +
                    "/" + std::to_string(preload_probe_offsets.size()) + "]"
            );

            render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Загрузка мира");
            return true;
        }

        render_world_transition_frames(1, "Загрузка мира");
    }

    if (platform_.should_close()) {
        return false;
    }

    const WorldStreamer::StreamingStats stats = world_streamer_->stats();
    log_message(
        LogLevel::Warning,
        std::string("Application: variant B preload timeout [visible=") +
            std::to_string(stats.visible_chunks) +
            ", spawn_loaded=" + (spawn_column_loaded_once ? "true" : "false") + "]"
    );

    return spawn_column_loaded_once;
}

'@

$ApplicationCppText = Replace-BetweenMarkers `
    $ApplicationCppText `
    "bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {" `
    "void Application::unload_world_for_menu() {" `
    $PreloadWorldSpawnFunction `
    "Application::preload_world_spawn variant B" `
    ([ref]$ApplicationCppChanged) `
    $Log

$UnloadWorldForMenuFunction = @'
void Application::unload_world_for_menu() {
    render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Выход из мира");

    if (world_streamer_ != nullptr) {
        world_streamer_->flush_all_dirty_chunks();
    }

    renderer_.unload_all_chunk_meshes();

    if (world_streamer_ != nullptr) {
        for (const ChunkCoord& coord : world_streamer_->drain_pending_unloads()) {
            renderer_.unload_chunk_mesh(coord);
        }

        world_streamer_.reset();
        world_save_.reset();
    }

    hovered_block_.reset();
    block_break_.target.reset();
    block_break_.repeat_seconds = 0.0f;

    render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Выход из мира");
}

'@

$ApplicationCppText = Replace-BetweenMarkers `
    $ApplicationCppText `
    "void Application::unload_world_for_menu() {" `
    "Renderer::CaveVisibilityFrame Application::update_cave_visibility_frame" `
    $UnloadWorldForMenuFunction `
    "Application::unload_world_for_menu complete renderer/world release" `
    ([ref]$ApplicationCppChanged) `
    $Log

if ($ApplicationCppChanged) {
    Write-Utf8NoBom $ApplicationCppPath $ApplicationCppText
    Add-Log $Log "OK: src/app/application.cpp written"
}

# ----------------------------------------------------------------------
# 4. Validation.
# ----------------------------------------------------------------------

$RuntimeTuningAfter = Read-Utf8Text $RuntimeTuningPath
$RendererHppAfter = Read-Utf8Text $RendererHppPath
$RendererCppAfter = Read-Utf8Text $RendererCppPath
$ApplicationCppAfter = Read-Utf8Text $ApplicationCppPath

if (-not $RuntimeTuningAfter.Contains("int spawn_preload_radius {2};")) {
    throw "Validation failed: desktop/practical spawn_preload_radius target missing"
}

if (-not $RuntimeTuningAfter.Contains("std::size_t spawn_preload_min_visible_chunks {25};")) {
    throw "Validation failed: spawn_preload_min_visible_chunks 25 missing"
}

if (-not $RendererHppAfter.Contains("void unload_all_chunk_meshes();")) {
    throw "Validation failed: renderer unload_all_chunk_meshes declaration missing"
}

if (-not $RendererCppAfter.Contains("void Renderer::unload_all_chunk_meshes()")) {
    throw "Validation failed: renderer unload_all_chunk_meshes implementation missing"
}

if (-not $ApplicationCppAfter.Contains("Application: variant B preload begin")) {
    throw "Validation failed: variant B preload function missing"
}

if (-not $ApplicationCppAfter.Contains("dx * kChunkWidth")) {
    throw "Validation failed: chunk-spaced preload probes missing"
}

if (-not $ApplicationCppAfter.Contains("renderer_.unload_all_chunk_meshes();")) {
    throw "Validation failed: complete renderer unload call missing"
}

if (-not $ApplicationCppAfter.Contains("world_streamer_->flush_all_dirty_chunks();")) {
    throw "Validation failed: world save flush missing before unload"
}

if (-not $ApplicationCppAfter.Contains("world_streamer_.reset();")) {
    throw "Validation failed: world_streamer reset missing"
}

if (-not $ApplicationCppAfter.Contains("world_save_.reset();")) {
    throw "Validation failed: world_save reset missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/render/renderer.hpp"
Add-Log $Log "- src/render/renderer.cpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Variant B loading: wait for safe 5x5 spawn area, not the full radius-16 world."
Add-Log $Log "- The player enters the world only after center-first preload reaches the safe area."
Add-Log $Log "- Normal streaming backlog continues loading the rest of radius 16 after entry."
Add-Log $Log "- Exit now flushes dirty chunks, unloads every resident renderer chunk mesh, then resets WorldStreamer and WorldSave."
Add-Log $Log "- This reduces long full-world loading while still preventing spawn falling and stale world memory."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Start a world and check logs for: Application: variant B preload done."
Add-Log $Log "3. Check that the player does not fall at spawn."
Add-Log $Log "4. Exit to menu and check that resident chunk meshes are cleared without rendering the world."
Add-Log $Log "5. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
