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

$RuntimeTuning = Join-Path $ProjectRoot "src\game\world_runtime_tuning.hpp"
$WorldStreamerHpp = Join-Path $ProjectRoot "src\game\world_streamer.hpp"
$WorldStreamerCpp = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$ApplicationCpp = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @($RuntimeTuning, $WorldStreamerHpp, $WorldStreamerCpp, $ApplicationCpp)
foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $RuntimeTuning
Backup-File $WorldStreamerHpp
Backup-File $WorldStreamerCpp
Backup-File $ApplicationCpp

# ----------------------------------------------------------------------
# 1. Runtime tuning: spawn preload nearest-first + visible black transition.
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
    int spawn_preload_radius {1};
    std::size_t spawn_preload_min_visible_chunks {9};
    int spawn_preload_max_frames {900};
    std::size_t spawn_preload_requests_per_frame {16};
    std::size_t spawn_preload_upload_max_count {16};
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
        std::size_t {4},
        600,
        std::size_t {8},
        std::size_t {8},
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
        1,
        std::size_t {9},
        900,
        std::size_t {16},
        std::size_t {16},
        30
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuning $runtimeTuningText
Add-Log $Log "OK: world_runtime_tuning.hpp rewritten"
Add-Log $Log "    - spawn_preload_radius = 1"
Add-Log $Log "    - spawn_preload_requests_per_frame = 16"
Add-Log $Log "    - transition_black_frames = 30"

# ----------------------------------------------------------------------
# 2. world_streamer.hpp: add request_spawn_preload public API.
# ----------------------------------------------------------------------

$hpp = Read-Utf8Text $WorldStreamerHpp
$hppChanged = $false

if (-not $hpp.Contains("void request_spawn_preload(Vec3 position, int radius, std::size_t max_requests);")) {
    $oldDecl = '    void tick_generation_jobs();'
    $newDecl = @'
    void request_spawn_preload(Vec3 position, int radius, std::size_t max_requests);
    void tick_generation_jobs();
'@

    if ($hpp.Contains($oldDecl)) {
        $hpp = $hpp.Replace($oldDecl, $newDecl.TrimEnd())
        $hppChanged = $true
        Add-Log $Log "OK: world_streamer.hpp added request_spawn_preload declaration"
    } else {
        throw "Cannot patch world_streamer.hpp: tick_generation_jobs declaration anchor not found"
    }
} else {
    Add-Log $Log "OK: request_spawn_preload declaration already exists"
}

if ($hppChanged) {
    Write-Utf8NoBom $WorldStreamerHpp $hpp
    Add-Log $Log "OK: src/game/world_streamer.hpp written"
}

# ----------------------------------------------------------------------
# 3. world_streamer.cpp: add nearest-first spawn preload.
# ----------------------------------------------------------------------

$cpp = Read-Utf8Text $WorldStreamerCpp
$cppChanged = $false

$spawnPreloadMethod = @'
void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests) {
    if (max_requests == 0) {
        return;
    }

    const ChunkCoord center = world_to_chunk(position);
    ++frame_counter_;
    observer_position_ = position;
    observer_chunk_ = center;
    last_streaming_update_position_ = position;
    has_streaming_update_position_ = true;
    previous_observer_position_ = position;
    has_previous_observer_position_ = true;
    observer_speed_blocks_per_second_ = 0.0f;

    const int preload_radius = std::clamp(radius, 0, chunk_radius_);
    std::vector<ChunkCoord> ordered_chunks;
    ordered_chunks.reserve(static_cast<std::size_t>((preload_radius * 2 + 1) * (preload_radius * 2 + 1)));

    for (int ring = 0; ring <= preload_radius; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                ordered_chunks.push_back({center.x + dx, center.z + dz});
            }
        }
    }

    std::stable_sort(
        ordered_chunks.begin(),
        ordered_chunks.end(),
        [&](const ChunkCoord& lhs, const ChunkCoord& rhs) {
            const int lhs_dx = lhs.x - center.x;
            const int lhs_dz = lhs.z - center.z;
            const int rhs_dx = rhs.x - center.x;
            const int rhs_dz = rhs.z - center.z;

            const int lhs_ring = std::max(std::abs(lhs_dx), std::abs(lhs_dz));
            const int rhs_ring = std::max(std::abs(rhs_dx), std::abs(rhs_dz));

            if (lhs_ring != rhs_ring) {
                return lhs_ring < rhs_ring;
            }

            const int lhs_dist = lhs_dx * lhs_dx + lhs_dz * lhs_dz;
            const int rhs_dist = rhs_dx * rhs_dx + rhs_dz * rhs_dz;

            if (lhs_dist != rhs_dist) {
                return lhs_dist < rhs_dist;
            }

            if (lhs.x != rhs.x) {
                return lhs.x < rhs.x;
            }

            return lhs.z < rhs.z;
        }
    );

    std::size_t requested = 0;

    for (const ChunkCoord& coord : ordered_chunks) {
        auto it = chunks_.find(coord);
        if (it != chunks_.end()) {
            it->second.last_touched_frame = frame_counter_;
            continue;
        }

        if (requested >= max_requests) {
            break;
        }

        {
            std::lock_guard lock(mutex_);
            if (job_queue_.size() >= max_job_queue_size()) {
                break;
            }
        }

        ChunkRecord record {};
        record.generation_version = next_chunk_version_++;
        record.mesh_version = 0;
        record.last_touched_frame = frame_counter_;
        const std::uint64_t version = record.generation_version;

        chunks_.emplace(coord, std::move(record));
        queue_generate_job(coord, version);
        ++requested;
    }

    {
        std::lock_guard lock(mutex_);
        std::stable_sort(
            job_queue_.begin(),
            job_queue_.end(),
            [&](const ChunkJob& lhs, const ChunkJob& rhs) {
                const int lhs_dx = lhs.coord.x - center.x;
                const int lhs_dz = lhs.coord.z - center.z;
                const int rhs_dx = rhs.coord.x - center.x;
                const int rhs_dz = rhs.coord.z - center.z;

                const int lhs_ring = std::max(std::abs(lhs_dx), std::abs(lhs_dz));
                const int rhs_ring = std::max(std::abs(rhs_dx), std::abs(rhs_dz));

                if (lhs_ring != rhs_ring) {
                    return lhs_ring < rhs_ring;
                }

                const int lhs_dist = lhs_dx * lhs_dx + lhs_dz * lhs_dz;
                const int rhs_dist = rhs_dx * rhs_dx + rhs_dz * rhs_dz;

                if (lhs_dist != rhs_dist) {
                    return lhs_dist < rhs_dist;
                }

                if (lhs.type != rhs.type) {
                    return static_cast<int>(lhs.type) < static_cast<int>(rhs.type);
                }

                if (lhs.coord.x != rhs.coord.x) {
                    return lhs.coord.x < rhs.coord.x;
                }

                return lhs.coord.z < rhs.coord.z;
            }
        );
    }

    refresh_visible_chunks();
    flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
}

'@

if (-not $cpp.Contains("void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests)")) {
    $marker = "void WorldStreamer::tick_generation_jobs() {"
    $idx = $cpp.IndexOf($marker)
    if ($idx -lt 0) {
        throw "Cannot insert request_spawn_preload: tick_generation_jobs marker not found"
    }

    $cpp = $cpp.Substring(0, $idx) + $spawnPreloadMethod + $cpp.Substring($idx)
    $cppChanged = $true
    Add-Log $Log "OK: world_streamer.cpp inserted nearest-first request_spawn_preload"
} else {
    $cpp = Replace-BetweenMarkers `
        $cpp `
        "void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests) {" `
        "void WorldStreamer::tick_generation_jobs() {" `
        $spawnPreloadMethod `
        "WorldStreamer::request_spawn_preload" `
        ([ref]$cppChanged) `
        $Log
}

if ($cppChanged) {
    Write-Utf8NoBom $WorldStreamerCpp $cpp
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
}

# ----------------------------------------------------------------------
# 4. application.cpp: replace preload_world_spawn so start uses center-out order.
# ----------------------------------------------------------------------

$app = Read-Utf8Text $ApplicationCpp
$appChanged = $false

$preloadFunction = @'
bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {
    if (world_streamer_ == nullptr) {
        return false;
    }

    render_black_transition_frames(world_runtime_tuning().transition_black_frames);

    const int spawn_block_x = static_cast<int>(std::floor(spawn_position.x));
    const int spawn_block_z = static_cast<int>(std::floor(spawn_position.z));
    const int probe_y = std::clamp(
        static_cast<int>(std::floor(spawn_position.y)) - 1,
        kWorldMinY,
        kWorldMaxY
    );

    constexpr std::array<std::array<int, 2>, 9> preload_probe_offsets {{
        {{0, 0}},
        {{1, 0}},
        {{-1, 0}},
        {{0, 1}},
        {{0, -1}},
        {{1, 1}},
        {{-1, 1}},
        {{1, -1}},
        {{-1, -1}}
    }};

    const int max_frames = world_runtime_tuning().spawn_preload_max_frames;
    const int preload_radius = world_runtime_tuning().spawn_preload_radius;
    const std::size_t min_visible_chunks = world_runtime_tuning().spawn_preload_min_visible_chunks;
    const std::size_t requests_per_frame = world_runtime_tuning().spawn_preload_requests_per_frame;
    const std::size_t upload_max_count = world_runtime_tuning().spawn_preload_upload_max_count;
    bool spawn_column_loaded_once = false;

    log_message(
        LogLevel::Info,
        std::string("Application: preload spawn chunks begin [radius=") +
            std::to_string(preload_radius) +
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
            loaded_probe_columns >= 5 &&
            stats.visible_chunks >= min_visible_chunks) {
            log_message(
                LogLevel::Info,
                std::string("Application: preload spawn chunks done [frame=") +
                    std::to_string(frame) +
                    ", visible=" + std::to_string(stats.visible_chunks) +
                    ", probes=" + std::to_string(loaded_probe_columns) + "]"
            );

            render_black_transition_frames(world_runtime_tuning().transition_black_frames);
            return true;
        }

        render_black_transition_frames(1);
    }

    if (platform_.should_close()) {
        return false;
    }

    const WorldStreamer::StreamingStats stats = world_streamer_->stats();
    log_message(
        LogLevel::Warning,
        std::string("Application: preload spawn chunks timeout; continuing [visible=") +
            std::to_string(stats.visible_chunks) +
            ", spawn_loaded=" + (spawn_column_loaded_once ? "true" : "false") + "]"
    );

    return spawn_column_loaded_once;
}

'@

$app = Replace-BetweenMarkers `
    $app `
    "bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {" `
    "void Application::unload_world_for_menu() {" `
    $preloadFunction `
    "Application::preload_world_spawn" `
    ([ref]$appChanged) `
    $Log

# Ensure pause exit still unloads before menu.
$pauseStart = $app.IndexOf("if (app_state_ == AppState::PauseMenu)")
if ($pauseStart -lt 0) {
    throw "Cannot find PauseMenu block"
}

$exitStart = $app.IndexOf("} else if (activated_button == kExitGameButtonIndex) {", $pauseStart)
if ($exitStart -lt 0) {
    throw "Cannot find PauseMenu exit branch"
}

$continueMarker = "                continue;"
$continueIndex = $app.IndexOf($continueMarker, $exitStart)
if ($continueIndex -lt 0) {
    throw "Cannot find PauseMenu exit continue marker"
}

$exitEnd = $continueIndex + $continueMarker.Length
$exitBlock = $app.Substring($exitStart, $exitEnd - $exitStart)

if (-not $exitBlock.Contains("unload_world_for_menu();")) {
    $newExitBlock = @'
} else if (activated_button == kExitGameButtonIndex) {
                platform_.play_ui_press_sound();
                unload_world_for_menu();
                platform_.set_mouse_capture(false);
                platform_.start_menu_music();
                app_state_ = AppState::MainMenu;
                selected_menu_button_ = 0;
                last_hovered_menu_button_ = -1;
                selected_pause_button_ = 0;
                pause_resume_waiting_for_jump_release_ = false;
                hovered_block_.reset();
                block_break_.target.reset();
                block_break_.repeat_seconds = 0.0f;
                continue;
'@
    $app = $app.Substring(0, $exitStart) + $newExitBlock + $app.Substring($exitEnd)
    $appChanged = $true
    Add-Log $Log "OK: PauseMenu exit now calls unload_world_for_menu"
} else {
    Add-Log $Log "OK: PauseMenu exit already calls unload_world_for_menu"
}

if ($appChanged) {
    Write-Utf8NoBom $ApplicationCpp $app
    Add-Log $Log "OK: src/app/application.cpp written"
}

# ----------------------------------------------------------------------
# 5. Validation.
# ----------------------------------------------------------------------

$rtAfter = Read-Utf8Text $RuntimeTuning
$hppAfter = Read-Utf8Text $WorldStreamerHpp
$cppAfter = Read-Utf8Text $WorldStreamerCpp
$appAfter = Read-Utf8Text $ApplicationCpp

if (-not $rtAfter.Contains("int spawn_preload_radius {1};")) {
    throw "Validation failed: spawn_preload_radius missing"
}

if (-not $rtAfter.Contains("std::size_t spawn_preload_requests_per_frame {16};")) {
    throw "Validation failed: spawn_preload_requests_per_frame missing"
}

if (-not $rtAfter.Contains("int transition_black_frames {30};")) {
    throw "Validation failed: transition_black_frames is not 30"
}

if (-not $hppAfter.Contains("void request_spawn_preload(Vec3 position, int radius, std::size_t max_requests);")) {
    throw "Validation failed: request_spawn_preload declaration missing"
}

if (-not $cppAfter.Contains("void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests)")) {
    throw "Validation failed: request_spawn_preload implementation missing"
}

if (-not $cppAfter.Contains("ordered_chunks.push_back({center.x + dx, center.z + dz});")) {
    throw "Validation failed: center-out spawn preload order missing"
}

if (-not $appAfter.Contains("world_streamer_->request_spawn_preload(spawn_position, preload_radius, requests_per_frame);")) {
    throw "Validation failed: application preload does not use request_spawn_preload"
}

if (-not $appAfter.Contains("render_black_transition_frames(world_runtime_tuning().transition_black_frames);")) {
    throw "Validation failed: black transition call missing"
}

$pauseStartAfter = $appAfter.IndexOf("if (app_state_ == AppState::PauseMenu)")
$exitStartAfter = $appAfter.IndexOf("} else if (activated_button == kExitGameButtonIndex) {", $pauseStartAfter)
$continueAfter = $appAfter.IndexOf("                continue;", $exitStartAfter)
if ($pauseStartAfter -lt 0 -or $exitStartAfter -lt 0 -or $continueAfter -lt 0) {
    throw "Validation failed: PauseMenu exit branch not found"
}
$patchedExitBlock = $appAfter.Substring($exitStartAfter, $continueAfter + "                continue;".Length - $exitStartAfter)
if (-not $patchedExitBlock.Contains("unload_world_for_menu();")) {
    throw "Validation failed: pause menu unload call missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/game/world_streamer.hpp"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Spawn preload no longer uses normal directional chunk priority."
Add-Log $Log "- Spawn preload requests chunks strictly center-out: player chunk first, then nearest ring."
Add-Log $Log "- The black transition now lasts 30 frames instead of 2."
Add-Log $Log "- During the black transition, the spawn area is generated, meshed and uploaded before InWorld control."
Add-Log $Log "- PauseMenu exit still saves/unloads world before returning to MainMenu."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Start a world and check logs for: Application: preload spawn chunks done."
Add-Log $Log "3. Confirm the player no longer falls before spawn chunks appear."
Add-Log $Log "4. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
