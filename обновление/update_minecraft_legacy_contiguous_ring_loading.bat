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
$WorldStreamerHppPath = Join-Path $ProjectRoot "src\game\world_streamer.hpp"
$WorldStreamerCppPath = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$RendererHppPath = Join-Path $ProjectRoot "src\render\renderer.hpp"
$RendererCppPath = Join-Path $ProjectRoot "src\render\renderer.cpp"
$ApplicationCppPath = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @(
    $RuntimeTuningPath,
    $WorldStreamerHppPath,
    $WorldStreamerCppPath,
    $RendererHppPath,
    $RendererCppPath,
    $ApplicationCppPath
)

foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $RuntimeTuningPath
Backup-File $WorldStreamerHppPath
Backup-File $WorldStreamerCppPath
Backup-File $RendererHppPath
Backup-File $RendererCppPath
Backup-File $ApplicationCppPath

# ----------------------------------------------------------------------
# 1. Runtime tuning: half selected distance, 2 sec loading/exit, contiguous rings.
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
    int spawn_preload_radius {0};
    std::size_t spawn_preload_min_visible_chunks {0};
    int spawn_preload_max_frames {1200};
    std::size_t spawn_preload_requests_per_frame {20};
    std::size_t spawn_preload_upload_max_count {20};
    std::size_t streaming_backlog_requests_per_frame {8};
    std::size_t world_exit_mesh_unload_budget_per_step {128};
    float preload_required_fraction {0.5f};
    float world_loading_min_seconds {2.0f};
    float world_leaving_min_seconds {2.0f};
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
        0,
        std::size_t {0},
        900,
        std::size_t {8},
        std::size_t {8},
        std::size_t {4},
        std::size_t {64},
        0.5f,
        2.0f,
        2.0f,
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
        0,
        std::size_t {0},
        1200,
        std::size_t {20},
        std::size_t {20},
        std::size_t {8},
        std::size_t {128},
        0.5f,
        2.0f,
        2.0f,
        30
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuningPath $RuntimeTuningText
Add-Log $Log "OK: world_runtime_tuning.hpp rewritten"

# ----------------------------------------------------------------------
# 2. WorldStreamer API: contiguous uploaded radius.
# ----------------------------------------------------------------------

$WorldStreamerHppText = Read-Utf8Text $WorldStreamerHppPath
$WorldStreamerHppChanged = $false

if (-not $WorldStreamerHppText.Contains("int continuous_uploaded_radius(Vec3 position, int max_radius) const;")) {
    $oldDecl = '    void request_spawn_preload(Vec3 position, int radius, std::size_t max_requests);'
    $newDecl = @'
    void request_spawn_preload(Vec3 position, int radius, std::size_t max_requests);
    int continuous_uploaded_radius(Vec3 position, int max_radius) const;
    bool all_chunks_uploaded_in_radius(Vec3 position, int radius) const;
'@
    if ($WorldStreamerHppText.Contains($oldDecl)) {
        $WorldStreamerHppText = $WorldStreamerHppText.Replace($oldDecl, $newDecl.TrimEnd())
        $WorldStreamerHppChanged = $true
        Add-Log $Log "OK: world_streamer.hpp added continuous radius API"
    } else {
        throw "Cannot patch world_streamer.hpp: request_spawn_preload declaration anchor not found"
    }
}

if ($WorldStreamerHppChanged) {
    Write-Utf8NoBom $WorldStreamerHppPath $WorldStreamerHppText
    Add-Log $Log "OK: src/game/world_streamer.hpp written"
}

# ----------------------------------------------------------------------
# 3. WorldStreamer implementation:
#    - update_observer queues only the first incomplete ring
#    - request_spawn_preload queues only the first incomplete ring
#    - refresh_visible_chunks hides chunks outside the continuous uploaded radius
# ----------------------------------------------------------------------

$WorldStreamerCppText = Read-Utf8Text $WorldStreamerCppPath
$WorldStreamerCppChanged = $false

$ContinuousMethods = @'
int WorldStreamer::continuous_uploaded_radius(Vec3 position, int max_radius) const {
    const ChunkCoord center = world_to_chunk(position);
    const int clamped_radius = std::clamp(max_radius, 0, chunk_radius_);

    std::lock_guard lock(mutex_);

    int continuous_radius = -1;
    for (int ring = 0; ring <= clamped_radius; ++ring) {
        bool ring_ready = true;

        for (int dz = -ring; dz <= ring && ring_ready; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {center.x + dx, center.z + dz};
                const auto it = chunks_.find(coord);
                if (it == chunks_.end() || !it->second.uploaded_to_gpu) {
                    ring_ready = false;
                    break;
                }
            }
        }

        if (!ring_ready) {
            break;
        }

        continuous_radius = ring;
    }

    return continuous_radius;
}

bool WorldStreamer::all_chunks_uploaded_in_radius(Vec3 position, int radius) const {
    return continuous_uploaded_radius(position, radius) >= radius;
}

'@

if (-not $WorldStreamerCppText.Contains("int WorldStreamer::continuous_uploaded_radius(Vec3 position, int max_radius) const")) {
    $insertMarker = "void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests) {"
    $insertAt = $WorldStreamerCppText.IndexOf($insertMarker)
    if ($insertAt -lt 0) {
        throw "Cannot insert continuous radius methods: request_spawn_preload marker not found"
    }
    $WorldStreamerCppText = $WorldStreamerCppText.Substring(0, $insertAt) + $ContinuousMethods + $WorldStreamerCppText.Substring($insertAt)
    $WorldStreamerCppChanged = $true
    Add-Log $Log "OK: world_streamer.cpp inserted continuous radius methods"
}

$UpdateObserverBlock = @'
void WorldStreamer::update_observer(Vec3 position) {
    update_observer(position, observer_forward_, 0.0f);
}

void WorldStreamer::update_observer(Vec3 position, Vec3 forward) {
    update_observer(position, forward, 0.0f);
}

void WorldStreamer::update_observer(Vec3 position, Vec3 forward, float dt_seconds) {
    const ChunkCoord origin = world_to_chunk(position);
    ++frame_counter_;
    observer_position_ = position;
    observer_forward_ = normalize({forward.x, 0.0f, forward.z});
    if (length(observer_forward_) <= 0.00001f) {
        observer_forward_ = {0.0f, 0.0f, -1.0f};
    }

    if (has_previous_observer_position_ && dt_seconds > 0.0001f) {
        const float dx = position.x - previous_observer_position_.x;
        const float dz = position.z - previous_observer_position_.z;
        const float instant_speed = std::sqrt(dx * dx + dz * dz) / dt_seconds;

        observer_speed_blocks_per_second_ =
            observer_speed_blocks_per_second_ * 0.85f +
            instant_speed * 0.15f;
    }

    previous_observer_position_ = position;
    has_previous_observer_position_ = true;
    observer_chunk_ = origin;
    last_streaming_update_position_ = position;
    has_streaming_update_position_ = true;

    streaming_backlog_origin_ = origin;
    streaming_backlog_.clear();
    streaming_backlog_cursor_ = 0;

    int active_ring = -1;
    for (int ring = 0; ring <= chunk_radius_; ++ring) {
        bool ring_ready = true;

        for (int dz = -ring; dz <= ring && ring_ready; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {origin.x + dx, origin.z + dz};
                const auto it = chunks_.find(coord);
                if (it == chunks_.end() || !it->second.uploaded_to_gpu) {
                    ring_ready = false;
                    break;
                }
            }
        }

        if (!ring_ready) {
            active_ring = ring;
            break;
        }
    }

    if (active_ring >= 0) {
        streaming_backlog_.reserve(static_cast<std::size_t>(active_ring == 0 ? 1 : active_ring * 8));

        for (int dz = -active_ring; dz <= active_ring; ++dz) {
            for (int dx = -active_ring; dx <= active_ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != active_ring) {
                    continue;
                }

                const ChunkCoord coord {origin.x + dx, origin.z + dz};
                auto it = chunks_.find(coord);
                if (it != chunks_.end()) {
                    it->second.last_touched_frame = frame_counter_;
                    continue;
                }

                streaming_backlog_.push_back(coord);
            }
        }

        std::sort(
            streaming_backlog_.begin(),
            streaming_backlog_.end(),
            [&](const ChunkCoord& lhs, const ChunkCoord& rhs) {
                const int lhs_dx = lhs.x - origin.x;
                const int lhs_dz = lhs.z - origin.z;
                const int rhs_dx = rhs.x - origin.x;
                const int rhs_dz = rhs.z - origin.z;

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
    }

    const std::size_t request_budget = std::min(
        max_new_chunk_requests_per_frame(),
        streaming_backlog_requests_per_frame()
    );

    std::size_t requested_this_frame = 0;

    while (streaming_backlog_cursor_ < streaming_backlog_.size() &&
           requested_this_frame < request_budget) {
        const ChunkCoord coord = streaming_backlog_[streaming_backlog_cursor_++];

        if (chunks_.find(coord) != chunks_.end()) {
            continue;
        }

        if (!desired_chunk(origin, coord)) {
            continue;
        }

        {
            std::lock_guard lock(mutex_);
            if (job_queue_.size() >= max_job_queue_size()) {
                if (streaming_backlog_cursor_ > 0) {
                    --streaming_backlog_cursor_;
                }
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
        ++requested_this_frame;
    }

    if (streaming_backlog_cursor_ >= streaming_backlog_.size()) {
        streaming_backlog_.clear();
        streaming_backlog_cursor_ = 0;
    }

    {
        std::lock_guard lock(mutex_);
        std::stable_sort(
            job_queue_.begin(),
            job_queue_.end(),
            [&](const ChunkJob& lhs, const ChunkJob& rhs) {
                const int lhs_dx = lhs.coord.x - origin.x;
                const int lhs_dz = lhs.coord.z - origin.z;
                const int rhs_dx = rhs.coord.x - origin.x;
                const int rhs_dz = rhs.coord.z - origin.z;

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

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (!desired_chunk(origin, it->first)) {
            const ChunkCoord unloaded_coord = it->first;
            if (world_save_ != nullptr && it->second.dirty_save && it->second.data.has_value()) {
                world_save_->save_chunk(unloaded_coord, *it->second.data);
            }
            {
                std::lock_guard lock(mutex_);
                const std::size_t before = job_queue_.size();
                std::erase_if(job_queue_, [&](const ChunkJob& job) {
                    return job.coord == unloaded_coord;
                });
                queued_light_jobs_.erase(unloaded_coord);
                dropped_jobs_ += before - job_queue_.size();
            }
            pending_unloads_.push_back(unloaded_coord);
            rebuild_states_.erase(unloaded_coord);
            dirty_save_set_.erase(unloaded_coord);
            if (logged_rebuild_lifecycle_count_ < 16) {
                log_message(
                    LogLevel::Info,
                    std::string("WorldStreamer: chunk unloaded coord=(") +
                        std::to_string(unloaded_coord.x) + "," + std::to_string(unloaded_coord.z) + ")"
                );
                ++logged_rebuild_lifecycle_count_;
            }
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }

    refresh_visible_chunks();
    flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
}

'@

$WorldStreamerCppText = Replace-BetweenMarkers `
    $WorldStreamerCppText `
    "void WorldStreamer::update_observer(Vec3 position) {" `
    "int WorldStreamer::continuous_uploaded_radius(Vec3 position, int max_radius) const" `
    $UpdateObserverBlock `
    "WorldStreamer::update_observer contiguous ring-first" `
    ([ref]$WorldStreamerCppChanged) `
    $Log

$RequestSpawnPreloadBlock = @'
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
    streaming_backlog_.clear();
    streaming_backlog_cursor_ = 0;
    streaming_backlog_origin_ = center;

    const int preload_radius = std::clamp(radius, 0, chunk_radius_);

    int active_ring = -1;
    for (int ring = 0; ring <= preload_radius; ++ring) {
        bool ring_ready = true;

        for (int dz = -ring; dz <= ring && ring_ready; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {center.x + dx, center.z + dz};
                const auto it = chunks_.find(coord);
                if (it == chunks_.end() || !it->second.uploaded_to_gpu) {
                    ring_ready = false;
                    break;
                }
            }
        }

        if (!ring_ready) {
            active_ring = ring;
            break;
        }
    }

    if (active_ring < 0) {
        refresh_visible_chunks();
        flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
        return;
    }

    std::vector<ChunkCoord> ordered_chunks;
    ordered_chunks.reserve(static_cast<std::size_t>(active_ring == 0 ? 1 : active_ring * 8));

    for (int dz = -active_ring; dz <= active_ring; ++dz) {
        for (int dx = -active_ring; dx <= active_ring; ++dx) {
            if (std::max(std::abs(dx), std::abs(dz)) != active_ring) {
                continue;
            }

            const ChunkCoord coord {center.x + dx, center.z + dz};
            auto it = chunks_.find(coord);
            if (it != chunks_.end()) {
                it->second.last_touched_frame = frame_counter_;
                continue;
            }

            ordered_chunks.push_back(coord);
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

$WorldStreamerCppText = Replace-BetweenMarkers `
    $WorldStreamerCppText `
    "void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests) {" `
    "void WorldStreamer::tick_generation_jobs() {" `
    $RequestSpawnPreloadBlock `
    "WorldStreamer::request_spawn_preload contiguous ring-only" `
    ([ref]$WorldStreamerCppChanged) `
    $Log

$RefreshVisibleChunksBlock = @'
void WorldStreamer::refresh_visible_chunks() {
    visible_chunks_.clear();

    int continuous_radius = -1;
    for (int ring = 0; ring <= chunk_radius_; ++ring) {
        bool ring_ready = true;

        for (int dz = -ring; dz <= ring && ring_ready; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {observer_chunk_.x + dx, observer_chunk_.z + dz};
                const auto it = chunks_.find(coord);
                if (it == chunks_.end() || !it->second.uploaded_to_gpu) {
                    ring_ready = false;
                    break;
                }
            }
        }

        if (!ring_ready) {
            break;
        }

        continuous_radius = ring;
    }

    for (const auto& [coord, record] : chunks_) {
        if (!record.uploaded_to_gpu || !desired_chunk(observer_chunk_, coord)) {
            continue;
        }

        const int dx = coord.x - observer_chunk_.x;
        const int dz = coord.z - observer_chunk_.z;
        const int ring = std::max(std::abs(dx), std::abs(dz));
        if (ring <= continuous_radius) {
            visible_chunks_.push_back({coord});
        }
    }

    std::sort(
        visible_chunks_.begin(),
        visible_chunks_.end(),
        [&](const ActiveChunk& lhs, const ActiveChunk& rhs) {
            const int lhs_dx = lhs.coord.x - observer_chunk_.x;
            const int lhs_dz = lhs.coord.z - observer_chunk_.z;
            const int rhs_dx = rhs.coord.x - observer_chunk_.x;
            const int rhs_dz = rhs.coord.z - observer_chunk_.z;

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

            if (lhs.coord.x != rhs.coord.x) {
                return lhs.coord.x < rhs.coord.x;
            }

            return lhs.coord.z < rhs.coord.z;
        }
    );
}

'@

$WorldStreamerCppText = Replace-BetweenMarkers `
    $WorldStreamerCppText `
    "void WorldStreamer::refresh_visible_chunks() {" `
    "std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads() {" `
    $RefreshVisibleChunksBlock `
    "WorldStreamer::refresh_visible_chunks continuous no-gap visibility" `
    ([ref]$WorldStreamerCppChanged) `
    $Log

if ($WorldStreamerCppChanged) {
    Write-Utf8NoBom $WorldStreamerCppPath $WorldStreamerCppText
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
}

# ----------------------------------------------------------------------
# 4. Renderer: no black cube and no menu buttons during loading/exit.
# ----------------------------------------------------------------------

$RendererHppText = Read-Utf8Text $RendererHppPath
$RendererHppChanged = $false

if (-not $RendererHppText.Contains("void draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message);")) {
    $oldDecl = '    void draw_main_menu(float time_seconds, bool use_night_panorama, int hovered_button);'
    $newDecl = @'
    void draw_main_menu(float time_seconds, bool use_night_panorama, int hovered_button);
    void draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message);
'@
    if ($RendererHppText.Contains($oldDecl)) {
        $RendererHppText = $RendererHppText.Replace($oldDecl, $newDecl.TrimEnd())
        $RendererHppChanged = $true
        Add-Log $Log "OK: renderer.hpp added draw_menu_panorama_message"
    } else {
        throw "Cannot patch renderer.hpp: draw_main_menu declaration anchor not found"
    }
}

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
        Add-Log $Log "OK: renderer.hpp added full unload API"
    } else {
        throw "Cannot patch renderer.hpp: unload_chunk_mesh declaration anchor not found"
    }
}

if ($RendererHppChanged) {
    Write-Utf8NoBom $RendererHppPath $RendererHppText
    Add-Log $Log "OK: src/render/renderer.hpp written"
}

$RendererCppText = Read-Utf8Text $RendererCppPath
$RendererCppChanged = $false

$RendererPanoramaMessageMethod = @'
void Renderer::draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message) {
    if (!frame_started_) {
        return;
    }

    update_main_menu_buffers(time_seconds, use_night_panorama, -1);

    const FrameResources& frame = frames_[current_frame_];
    const MenuTexture& panorama = use_night_panorama ? menu_panorama_night_ : menu_panorama_day_;

    if (menu_panorama_vertex_count_ > 0 && panorama.descriptor_set != VK_NULL_HANDLE) {
        draw_textured_buffer(frame, menu_panorama_vertex_buffer_, menu_panorama_vertex_count_, panorama.descriptor_set);
    }

    const VkExtent2D extent = logical_extent();
    const float width = static_cast<float>(extent.width == 0 ? 1 : extent.width);
    const float height = static_cast<float>(extent.height == 0 ? 1 : extent.height);

    if (menu_font_.loaded && !message.empty()) {
        constexpr float text_height = 30.0f;
        const float text_width = menu_font_text_width(message, text_height);
        const float text_x = (width - text_width) * 0.5f;
        const float text_y = (height - text_height) * 0.5f;

        std::vector<Vertex> text_vertices;

        append_menu_font_text(
            text_vertices,
            message,
            text_x + 2.0f,
            text_y + 2.0f,
            text_height,
            width,
            height,
            {0.0f, 0.0f, 0.0f}
        );

        append_menu_font_text(
            text_vertices,
            message,
            text_x,
            text_y,
            text_height,
            width,
            height,
            {1.0f, 1.0f, 1.0f}
        );

        upload_dynamic_buffer(menu_font_vertex_buffer_, text_vertices);
        menu_font_vertex_count_ = static_cast<std::uint32_t>(text_vertices.size());
        if (menu_font_vertex_count_ > 0 && menu_font_.texture.descriptor_set != VK_NULL_HANDLE) {
            draw_textured_buffer(frame, menu_font_vertex_buffer_, menu_font_vertex_count_, menu_font_.texture.descriptor_set);
        }
    }
}

'@

if ($RendererCppText.Contains("void Renderer::draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message)")) {
    $RendererCppText = Replace-BetweenMarkers `
        $RendererCppText `
        "void Renderer::draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message) {" `
        "void Renderer::draw_pause_menu" `
        $RendererPanoramaMessageMethod `
        "Renderer::draw_menu_panorama_message no buttons/no black cube" `
        ([ref]$RendererCppChanged) `
        $Log
} else {
    $insertMarker = "void Renderer::draw_pause_menu"
    $insertAt = $RendererCppText.IndexOf($insertMarker)
    if ($insertAt -lt 0) {
        $insertMarker = "bool Renderer::upload_chunk_mesh"
        $insertAt = $RendererCppText.IndexOf($insertMarker)
    }
    if ($insertAt -lt 0) {
        throw "Cannot insert draw_menu_panorama_message: marker not found"
    }
    $RendererCppText = $RendererCppText.Substring(0, $insertAt) + $RendererPanoramaMessageMethod + $RendererCppText.Substring($insertAt)
    $RendererCppChanged = $true
    Add-Log $Log "OK: renderer.cpp inserted draw_menu_panorama_message"
}

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
        throw "Cannot insert unload_all_chunk_meshes: marker not found"
    }
    $RendererCppText = $RendererCppText.Substring(0, $insertAt) + $RendererUnloadAllMethod + $RendererCppText.Substring($insertAt)
    $RendererCppChanged = $true
    Add-Log $Log "OK: renderer.cpp inserted unload_all_chunk_meshes"
}

if ($RendererCppChanged) {
    Write-Utf8NoBom $RendererCppPath $RendererCppText
    Add-Log $Log "OK: src/render/renderer.cpp written"
}

# ----------------------------------------------------------------------
# 5. Application: half-distance contiguous loading and 2 sec exit.
# ----------------------------------------------------------------------

$ApplicationCppText = Read-Utf8Text $ApplicationCppPath
$ApplicationCppChanged = $false

$TransitionHelpers = @'
void Application::render_black_transition_frames(int frame_count) {
    render_world_transition_frames(frame_count, "Загрузка мира");
}

void Application::render_world_transition_frames(int frame_count, const char* message) {
    const CameraFrameData loading_camera {
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        {},
        {0.0f, 0.0f, -1.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}
    };

    static auto previous_transition_time = std::chrono::steady_clock::now();

    for (int i = 0; i < frame_count && !platform_.should_close(); ++i) {
        platform_.pump_events();

        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - previous_transition_time).count();
        previous_transition_time = now;
        menu_time_seconds_ += dt > 0.0001f ? std::min(dt, 0.1f) : (1.0f / 60.0f);

        renderer_.begin_frame(loading_camera);
        renderer_.draw_menu_panorama_message(
            menu_time_seconds_,
            menu_uses_night_panorama_,
            message != nullptr ? message : ""
        );
        renderer_.end_frame();
    }
}

'@

$ApplicationCppText = Replace-BetweenMarkers `
    $ApplicationCppText `
    "void Application::render_black_transition_frames(int frame_count) {" `
    "bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {" `
    $TransitionHelpers `
    "Application transition helpers no button flicker" `
    ([ref]$ApplicationCppChanged) `
    $Log

$PreloadWorldSpawnFunction = @'
bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {
    if (world_streamer_ == nullptr) {
        return false;
    }

    render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Загрузка мира");

    const auto loading_started = std::chrono::steady_clock::now();

    const int selected_radius = std::max(1, world_streamer_->chunk_radius());
    const int required_preload_radius = std::clamp(
        static_cast<int>(std::ceil(static_cast<float>(selected_radius) * world_runtime_tuning().preload_required_fraction)),
        1,
        selected_radius
    );

    const std::size_t requests_per_frame = world_runtime_tuning().spawn_preload_requests_per_frame;
    const std::size_t upload_max_count = world_runtime_tuning().spawn_preload_upload_max_count;

    bool required_area_ready_once = false;
    bool warning_logged = false;

    log_message(
        LogLevel::Info,
        std::string("Application: contiguous half-distance preload begin [selected_radius=") +
            std::to_string(selected_radius) +
            ", required_radius=" + std::to_string(required_preload_radius) +
            ", min_seconds=" + std::to_string(world_runtime_tuning().world_loading_min_seconds) + "]"
    );

    for (int frame = 0; !platform_.should_close(); ++frame) {
        platform_.pump_events();

        const auto now = std::chrono::steady_clock::now();
        const float elapsed_seconds = std::chrono::duration<float>(now - loading_started).count();

        const int active_preload_radius = required_area_ready_once ? selected_radius : required_preload_radius;
        world_streamer_->request_spawn_preload(spawn_position, active_preload_radius, requests_per_frame);

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

        const int continuous_radius = world_streamer_->continuous_uploaded_radius(spawn_position, required_preload_radius);
        required_area_ready_once = continuous_radius >= required_preload_radius;

        if (required_area_ready_once &&
            elapsed_seconds >= world_runtime_tuning().world_loading_min_seconds) {
            log_message(
                LogLevel::Info,
                std::string("Application: contiguous half-distance preload done [frame=") +
                    std::to_string(frame) +
                    ", elapsed=" + std::to_string(elapsed_seconds) +
                    ", selected_radius=" + std::to_string(selected_radius) +
                    ", required_radius=" + std::to_string(required_preload_radius) +
                    ", continuous_radius=" + std::to_string(continuous_radius) + "]"
            );

            return true;
        }

        if (!warning_logged && frame >= world_runtime_tuning().spawn_preload_max_frames) {
            warning_logged = true;
            log_message(
                LogLevel::Warning,
                std::string("Application: contiguous preload is taking longer than expected [frame=") +
                    std::to_string(frame) +
                    ", continuous_radius=" + std::to_string(continuous_radius) +
                    ", required_radius=" + std::to_string(required_preload_radius) + "]"
            );
        }

        render_world_transition_frames(1, "Загрузка мира");
    }

    return false;
}

'@

$ApplicationCppText = Replace-BetweenMarkers `
    $ApplicationCppText `
    "bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {" `
    "void Application::unload_world_for_menu() {" `
    $PreloadWorldSpawnFunction `
    "Application::preload_world_spawn contiguous half distance" `
    ([ref]$ApplicationCppChanged) `
    $Log

$UnloadWorldForMenuFunction = @'
void Application::unload_world_for_menu() {
    const auto leaving_started = std::chrono::steady_clock::now();

    render_world_transition_frames(1, "Выход из мира");

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

    while (!platform_.should_close()) {
        const float elapsed_seconds = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - leaving_started
        ).count();

        if (elapsed_seconds >= world_runtime_tuning().world_leaving_min_seconds &&
            world_streamer_ == nullptr &&
            world_save_ == nullptr &&
            renderer_.resident_chunk_mesh_count() == 0) {
            break;
        }

        render_world_transition_frames(1, "Выход из мира");
    }
}

'@

$ApplicationCppText = Replace-BetweenMarkers `
    $ApplicationCppText `
    "void Application::unload_world_for_menu() {" `
    "Renderer::CaveVisibilityFrame Application::update_cave_visibility_frame" `
    $UnloadWorldForMenuFunction `
    "Application::unload_world_for_menu 2sec full cleanup" `
    ([ref]$ApplicationCppChanged) `
    $Log

if ($ApplicationCppChanged) {
    Write-Utf8NoBom $ApplicationCppPath $ApplicationCppText
    Add-Log $Log "OK: src/app/application.cpp written"
}

# ----------------------------------------------------------------------
# 6. Validation.
# ----------------------------------------------------------------------

$RuntimeTuningAfter = Read-Utf8Text $RuntimeTuningPath
$WorldStreamerHppAfter = Read-Utf8Text $WorldStreamerHppPath
$WorldStreamerCppAfter = Read-Utf8Text $WorldStreamerCppPath
$RendererHppAfter = Read-Utf8Text $RendererHppPath
$RendererCppAfter = Read-Utf8Text $RendererCppPath
$ApplicationCppAfter = Read-Utf8Text $ApplicationCppPath

if (-not $RuntimeTuningAfter.Contains("float preload_required_fraction {0.5f};")) {
    throw "Validation failed: preload_required_fraction missing"
}

if (-not $WorldStreamerHppAfter.Contains("int continuous_uploaded_radius(Vec3 position, int max_radius) const;")) {
    throw "Validation failed: continuous_uploaded_radius declaration missing"
}

if (-not $WorldStreamerCppAfter.Contains("int WorldStreamer::continuous_uploaded_radius(Vec3 position, int max_radius) const")) {
    throw "Validation failed: continuous_uploaded_radius implementation missing"
}

if (-not $WorldStreamerCppAfter.Contains("active_ring = ring;")) {
    throw "Validation failed: active ring generation missing"
}

if (-not $WorldStreamerCppAfter.Contains("ring <= continuous_radius")) {
    throw "Validation failed: visible chunks are not limited to continuous radius"
}

if ($RendererCppAfter.Contains("height * 0.42f") -and $RendererCppAfter.Contains("draw_menu_panorama_message")) {
    throw "Validation failed: black overlay/cube still appears in panorama transition method"
}

if (-not $RendererCppAfter.Contains("text_x + 2.0f")) {
    throw "Validation failed: text shadow missing"
}

if (-not $ApplicationCppAfter.Contains("continuous_uploaded_radius(spawn_position, required_preload_radius)")) {
    throw "Validation failed: loading does not wait for continuous uploaded radius"
}

if (-not $ApplicationCppAfter.Contains("world_loading_min_seconds")) {
    throw "Validation failed: 2 second loading gate missing"
}

if (-not $ApplicationCppAfter.Contains("world_leaving_min_seconds")) {
    throw "Validation failed: 2 second leaving gate missing"
}

if (-not $ApplicationCppAfter.Contains("renderer_.resident_chunk_mesh_count() == 0")) {
    throw "Validation failed: renderer cleanup check missing"
}

if (-not $ApplicationCppAfter.Contains("renderer_.unload_all_chunk_meshes();")) {
    throw "Validation failed: renderer full unload call missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/game/world_streamer.hpp"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log "- src/render/renderer.hpp"
Add-Log $Log "- src/render/renderer.cpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Chunk generation is now contiguous ring-first from the player."
Add-Log $Log "- Loading waits for continuous uploaded radius = ceil(selected_chunk_radius * 0.5)."
Add-Log $Log "- Far chunks are hidden until all rings between player and that chunk are uploaded."
Add-Log $Log "- The normal in-world streamer queues only the first incomplete ring."
Add-Log $Log "- Loading/exit screens use panorama + text, no black cube."
Add-Log $Log "- Menu buttons are not drawn by the transition method."
Add-Log $Log "- Exit waits at least 2 seconds and checks renderer/world cleanup."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Start world with radius 16 and verify log required_radius=8."
Add-Log $Log "3. Confirm terrain appears as a solid carpet from player outward, not as islands."
Add-Log $Log "4. Confirm menu buttons do not flicker during loading/exit."
Add-Log $Log "5. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
