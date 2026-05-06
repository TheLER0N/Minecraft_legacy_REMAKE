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

$RuntimeTuningPath = Join-Path $ProjectRoot "src\game\world_runtime_tuning.hpp"
$WorldStreamerCppPath = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$RendererCppPath = Join-Path $ProjectRoot "src\render\renderer.cpp"

$RequiredFiles = @(
    $RuntimeTuningPath,
    $WorldStreamerCppPath,
    $RendererCppPath
)

foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $RuntimeTuningPath
Backup-File $WorldStreamerCppPath
Backup-File $RendererCppPath

# ----------------------------------------------------------------------
# 1. Runtime tuning.
#    Goal: normal world shape, corridor as priority only, conservative budget.
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
    int keep_radius_extra_chunks {3};
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
    std::size_t spawn_preload_requests_per_frame {18};
    std::size_t spawn_preload_upload_max_count {14};

    std::size_t streaming_backlog_requests_per_frame {8};
    std::size_t world_exit_mesh_unload_budget_per_step {128};
    int contiguous_generation_ring_window {3};

    float preload_required_fraction {0.5f};
    float world_loading_min_seconds {2.0f};
    float world_leaving_min_seconds {2.0f};
    int transition_black_frames {30};

    // Fast-flight priority tuning. These values affect priority only, not world visibility mask.
    bool adaptive_corridor_streaming_enabled {true};
    float corridor_speed_threshold_blocks_per_second {30.0f};
    float elytra_expected_speed_blocks_per_second {78.4f};
    float corridor_lookahead_seconds {2.25f};
    float corridor_velocity_weight {0.75f};
    float corridor_look_weight {0.25f};

    int corridor_safe_radius_chunks {2};
    int corridor_rear_keep_chunks {4};
    int corridor_min_forward_chunks {8};
    int corridor_max_forward_chunks {16};
    int corridor_inner_half_width_chunks {3};
    int corridor_outer_half_width_chunks {5};
    int corridor_generation_ahead_chunks {4};

    std::size_t corridor_requests_per_frame {20};
    std::size_t corridor_uploads_per_frame {12};
    std::size_t corridor_upload_byte_budget {5ull * 1024ull * 1024ull};
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
        2,
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
        2,

        0.5f,
        2.0f,
        2.0f,
        30,

        true,
        28.0f,
        78.4f,
        2.0f,
        0.75f,
        0.25f,

        2,
        3,
        6,
        10,
        2,
        4,
        2,

        std::size_t {8},
        std::size_t {8},
        2ull * 1024ull * 1024ull
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
        std::clamp<std::size_t>(worker_count * 10, std::size_t {48}, std::size_t {128}),
        std::clamp<std::size_t>(worker_count * 96, std::size_t {384}, std::size_t {1536}),
        3ull * 1024ull * 1024ull,
        5ull * 1024ull * 1024ull,
        std::clamp<std::size_t>(worker_count / 2, std::size_t {2}, std::size_t {5}),
        1.5f,

        16,
        3,
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
        std::size_t {18},
        std::size_t {14},

        std::size_t {8},
        std::size_t {128},
        3,

        0.5f,
        2.0f,
        2.0f,
        30,

        true,
        30.0f,
        78.4f,
        2.25f,
        0.75f,
        0.25f,

        2,
        4,
        8,
        16,
        3,
        5,
        4,

        std::size_t {20},
        std::size_t {12},
        5ull * 1024ull * 1024ull
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuningPath $RuntimeTuningText
Add-Log $Log "OK: src/game/world_runtime_tuning.hpp rewritten"

# ----------------------------------------------------------------------
# 2. WorldStreamer: clean helper block.
# ----------------------------------------------------------------------

$WorldText = Read-Utf8Text $WorldStreamerCppPath
$WorldChanged = $false

# Ensure needed standard header is present. The file already uses vector later, but keep the TU self-contained.
if (-not $WorldText.Contains("#include <vector>")) {
    $WorldText = $WorldText.Replace("#include <utility>", "#include <utility>`r`n#include <vector>")
    $WorldChanged = $true
    Add-Log $Log "OK: added <vector> include"
}

$CleanHelperBlock = @'
std::size_t max_new_chunk_requests_per_frame() {
    return world_runtime_tuning().max_new_chunk_requests_per_frame;
}

std::size_t max_completed_results_per_tick() {
    return world_runtime_tuning().max_completed_results_per_tick;
}

std::size_t max_job_queue_size() {
    return world_runtime_tuning().max_job_queue_size;
}

std::size_t streaming_backlog_requests_per_frame() {
    return world_runtime_tuning().streaming_backlog_requests_per_frame;
}

std::size_t corridor_requests_per_frame() {
    return world_runtime_tuning().corridor_requests_per_frame;
}

std::size_t corridor_uploads_per_frame() {
    return world_runtime_tuning().corridor_uploads_per_frame;
}

std::size_t corridor_upload_byte_budget() {
    return world_runtime_tuning().corridor_upload_byte_budget;
}

int contiguous_generation_ring_window() {
    return std::max(1, world_runtime_tuning().contiguous_generation_ring_window);
}

int keep_radius_extra_chunks() {
    return std::max(0, world_runtime_tuning().keep_radius_extra_chunks);
}

float completed_result_apply_budget_ms() {
    return world_runtime_tuning().completed_result_apply_budget_ms;
}

int target_chunk_radius() {
    return world_runtime_tuning().target_chunk_radius;
}

float streaming_update_distance_blocks() {
    return world_runtime_tuning().streaming_update_distance_blocks;
}

float forward_priority_weight() {
    return world_runtime_tuning().forward_priority_weight;
}

float side_priority_weight() {
    return world_runtime_tuning().side_priority_weight;
}

float back_priority_penalty() {
    return world_runtime_tuning().back_priority_penalty;
}

int min_forward_buffer_chunks() {
    return world_runtime_tuning().min_forward_buffer_chunks;
}

int max_forward_buffer_chunks() {
    return world_runtime_tuning().max_forward_buffer_chunks;
}

int min_forward_width_chunks() {
    return world_runtime_tuning().min_forward_width_chunks;
}

int max_forward_width_chunks() {
    return world_runtime_tuning().max_forward_width_chunks;
}

float forward_buffer_pipeline_seconds() {
    return world_runtime_tuning().forward_buffer_pipeline_seconds;
}

float forward_buffer_safety_blocks() {
    return world_runtime_tuning().forward_buffer_safety_blocks;
}

float fast_flight_speed_threshold() {
    return world_runtime_tuning().fast_flight_speed_threshold;
}

float very_fast_flight_speed_threshold() {
    return world_runtime_tuning().very_fast_flight_speed_threshold;
}

bool adaptive_corridor_streaming_enabled() {
    return world_runtime_tuning().adaptive_corridor_streaming_enabled;
}

float corridor_speed_threshold_blocks_per_second() {
    return world_runtime_tuning().corridor_speed_threshold_blocks_per_second;
}

int corridor_safe_radius_chunks() {
    return std::max(1, world_runtime_tuning().corridor_safe_radius_chunks);
}

int corridor_rear_keep_chunks() {
    return std::max(0, world_runtime_tuning().corridor_rear_keep_chunks);
}

int corridor_inner_half_width_chunks() {
    return std::max(1, world_runtime_tuning().corridor_inner_half_width_chunks);
}

int corridor_outer_half_width_chunks() {
    return std::max(corridor_inner_half_width_chunks(), world_runtime_tuning().corridor_outer_half_width_chunks);
}

bool corridor_mode_for_speed(float speed_blocks_per_second) {
    return adaptive_corridor_streaming_enabled() &&
        speed_blocks_per_second >= corridor_speed_threshold_blocks_per_second();
}

int adaptive_corridor_forward_chunks(float speed_blocks_per_second, int chunk_radius) {
    const float chunks_per_second = speed_blocks_per_second / static_cast<float>(kChunkWidth);
    const int calculated = static_cast<int>(
        std::ceil(chunks_per_second * world_runtime_tuning().corridor_lookahead_seconds + 2.0f)
    );

    return std::clamp(
        calculated,
        std::max(1, world_runtime_tuning().corridor_min_forward_chunks),
        std::min(chunk_radius, world_runtime_tuning().corridor_max_forward_chunks)
    );
}

int adaptive_forward_buffer_chunks(float speed_blocks_per_second) {
    const float needed_blocks =
        speed_blocks_per_second * forward_buffer_pipeline_seconds() +
        forward_buffer_safety_blocks();

    const int calculated = static_cast<int>(std::ceil(needed_blocks / static_cast<float>(kChunkWidth)));
    return std::clamp(calculated, min_forward_buffer_chunks(), max_forward_buffer_chunks());
}

int adaptive_forward_width_chunks(float speed_blocks_per_second) {
    int width = min_forward_width_chunks();

    if (speed_blocks_per_second >= very_fast_flight_speed_threshold()) {
        width = max_forward_width_chunks();
    } else if (speed_blocks_per_second >= fast_flight_speed_threshold()) {
        width = std::min(max_forward_width_chunks(), min_forward_width_chunks() + 2);
    }

    if ((width % 2) == 0) {
        ++width;
    }

    return std::clamp(width, min_forward_width_chunks(), max_forward_width_chunks());
}

Vec3 normalized_horizontal_direction(Vec3 direction) {
    direction.y = 0.0f;
    if (length(direction) <= 0.00001f) {
        return {0.0f, 0.0f, -1.0f};
    }
    return normalize(direction);
}

float chunk_forward_units(ChunkCoord origin, ChunkCoord coord, Vec3 direction) {
    const float dx = static_cast<float>(coord.x - origin.x);
    const float dz = static_cast<float>(coord.z - origin.z);
    return dx * direction.x + dz * direction.z;
}

float chunk_side_units(ChunkCoord origin, ChunkCoord coord, Vec3 direction) {
    const float dx = static_cast<float>(coord.x - origin.x);
    const float dz = static_cast<float>(coord.z - origin.z);
    return std::abs(dx * -direction.z + dz * direction.x);
}

bool load_area_chunk(ChunkCoord origin, ChunkCoord coord, int chunk_radius) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    return std::max(std::abs(dx), std::abs(dz)) <= chunk_radius;
}

bool keep_area_chunk(ChunkCoord origin, ChunkCoord coord, int chunk_radius) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    const int keep_radius = std::min(kMaxChunkRadius, chunk_radius + keep_radius_extra_chunks());
    return std::max(std::abs(dx), std::abs(dz)) <= keep_radius;
}

// Compatibility helper. This is NOT a forward-corridor world mask.
// Corridor logic must affect priority only.
bool corridor_candidate_chunk(
    ChunkCoord origin,
    ChunkCoord coord,
    Vec3 direction,
    float speed_blocks_per_second,
    int chunk_radius
) {
    (void)direction;
    (void)speed_blocks_per_second;
    return load_area_chunk(origin, coord, chunk_radius);
}

float corridor_priority_score(
    ChunkCoord origin,
    ChunkCoord coord,
    Vec3 direction,
    float speed_blocks_per_second
) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    const int chebyshev = std::max(std::abs(dx), std::abs(dz));
    const int dist_sq = dx * dx + dz * dz;

    if (chebyshev == 0) {
        return 0.0f;
    }

    if (chebyshev <= corridor_safe_radius_chunks()) {
        return 5.0f + static_cast<float>(dist_sq);
    }

    const float forward = chunk_forward_units(origin, coord, direction);
    const float side = chunk_side_units(origin, coord, direction);

    if (!corridor_mode_for_speed(speed_blocks_per_second)) {
        const float behind = std::max(0.0f, -forward) * 8.0f;
        const float look_bonus = std::max(0.0f, forward) * -1.5f;
        return 80.0f + static_cast<float>(dist_sq) * 0.75f + side * 4.0f + behind + look_bonus;
    }

    const int forward_chunks = adaptive_corridor_forward_chunks(speed_blocks_per_second, world_runtime_tuning().target_chunk_radius);
    const float clamped_forward = std::clamp(
        forward,
        0.0f,
        static_cast<float>(forward_chunks)
    );

    const float behind_penalty = forward < -static_cast<float>(corridor_rear_keep_chunks())
        ? 650.0f + std::abs(forward) * 45.0f
        : std::max(0.0f, -forward) * 18.0f;

    return 100.0f +
        static_cast<float>(dist_sq) * 0.45f +
        side * 22.0f +
        behind_penalty -
        clamped_forward * 10.0f;
}

constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;
'@

$WorldText = Replace-BetweenMarkers `
    $WorldText `
    "std::size_t max_new_chunk_requests_per_frame() {" `
    "constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;" `
    $CleanHelperBlock `
    "WorldStreamer helper block cleanup" `
    ([ref]$WorldChanged) `
    $Log

# ----------------------------------------------------------------------
# 3. WorldStreamer update_observer.
# ----------------------------------------------------------------------

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

    Vec3 camera_forward = normalized_horizontal_direction({forward.x, 0.0f, forward.z});
    Vec3 motion_direction = camera_forward;

    if (has_previous_observer_position_ && dt_seconds > 0.0001f) {
        const float dx = position.x - previous_observer_position_.x;
        const float dz = position.z - previous_observer_position_.z;
        const float instant_speed = std::sqrt(dx * dx + dz * dz) / dt_seconds;

        observer_speed_blocks_per_second_ =
            observer_speed_blocks_per_second_ * 0.85f +
            instant_speed * 0.15f;

        if (std::sqrt(dx * dx + dz * dz) > 0.001f) {
            motion_direction = normalized_horizontal_direction({dx, 0.0f, dz});
        }
    }

    const bool corridor_mode = corridor_mode_for_speed(observer_speed_blocks_per_second_);
    Vec3 stream_direction = camera_forward;
    if (corridor_mode) {
        stream_direction = normalized_horizontal_direction({
            motion_direction.x * world_runtime_tuning().corridor_velocity_weight +
                camera_forward.x * world_runtime_tuning().corridor_look_weight,
            0.0f,
            motion_direction.z * world_runtime_tuning().corridor_velocity_weight +
                camera_forward.z * world_runtime_tuning().corridor_look_weight
        });
    }

    observer_forward_ = stream_direction;
    previous_observer_position_ = position;
    has_previous_observer_position_ = true;
    observer_chunk_ = origin;

    const float streaming_distance = streaming_update_distance_blocks();
    const float move_dx = position.x - last_streaming_update_position_.x;
    const float move_dz = position.z - last_streaming_update_position_.z;
    const float move_distance_sq = move_dx * move_dx + move_dz * move_dz;
    const float required_distance_sq = streaming_distance * streaming_distance;

    const bool changed_chunk =
        !has_streaming_update_position_ ||
        streaming_backlog_origin_.x != origin.x ||
        streaming_backlog_origin_.z != origin.z;

    const bool should_rebuild_backlog =
        !has_streaming_update_position_ ||
        move_distance_sq >= required_distance_sq ||
        streaming_backlog_.empty() ||
        changed_chunk;

    if (should_rebuild_backlog) {
        last_streaming_update_position_ = position;
        has_streaming_update_position_ = true;
        streaming_backlog_origin_ = origin;
        streaming_backlog_.clear();
        streaming_backlog_cursor_ = 0;
        streaming_backlog_.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));

        for (int dz = -chunk_radius_; dz <= chunk_radius_; ++dz) {
            for (int dx = -chunk_radius_; dx <= chunk_radius_; ++dx) {
                const ChunkCoord coord {origin.x + dx, origin.z + dz};

                if (!load_area_chunk(origin, coord, chunk_radius_)) {
                    continue;
                }

                if (auto it = chunks_.find(coord); it != chunks_.end()) {
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
                const float lhs_score = corridor_priority_score(
                    origin,
                    lhs,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );
                const float rhs_score = corridor_priority_score(
                    origin,
                    rhs,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );

                if (lhs_score != rhs_score) {
                    return lhs_score < rhs_score;
                }

                if (lhs.x != rhs.x) {
                    return lhs.x < rhs.x;
                }

                return lhs.z < rhs.z;
            }
        );
    } else {
        for (auto& [coord, record] : chunks_) {
            if (keep_area_chunk(origin, coord, chunk_radius_)) {
                record.last_touched_frame = frame_counter_;
            }
        }
    }

    const std::size_t request_budget = corridor_mode
        ? std::min(corridor_requests_per_frame(), max_new_chunk_requests_per_frame())
        : std::min(max_new_chunk_requests_per_frame(), streaming_backlog_requests_per_frame());

    std::size_t requested_this_frame = 0;

    while (streaming_backlog_cursor_ < streaming_backlog_.size() &&
           requested_this_frame < request_budget) {
        const ChunkCoord coord = streaming_backlog_[streaming_backlog_cursor_++];

        if (chunks_.find(coord) != chunks_.end()) {
            continue;
        }

        if (!load_area_chunk(origin, coord, chunk_radius_)) {
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

    if (requested_this_frame > 0 || should_rebuild_backlog) {
        std::lock_guard lock(mutex_);
        std::stable_sort(
            job_queue_.begin(),
            job_queue_.end(),
            [&](const ChunkJob& lhs, const ChunkJob& rhs) {
                const float lhs_score = corridor_priority_score(
                    origin,
                    lhs.coord,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );
                const float rhs_score = corridor_priority_score(
                    origin,
                    rhs.coord,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );

                if (lhs_score != rhs_score) {
                    return lhs_score < rhs_score;
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
        if (!keep_area_chunk(origin, it->first, chunk_radius_)) {
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

$WorldText = Replace-BetweenMarkers `
    $WorldText `
    "void WorldStreamer::update_observer(Vec3 position) {" `
    "int WorldStreamer::continuous_uploaded_radius" `
    $UpdateObserverBlock `
    "WorldStreamer::update_observer stable priority loader" `
    ([ref]$WorldChanged) `
    $Log

# ----------------------------------------------------------------------
# 4. continuous_uploaded_radius + all_chunks_uploaded_in_radius.
# ----------------------------------------------------------------------

$ContinuousBlock = @'
int WorldStreamer::continuous_uploaded_radius(Vec3 position, int max_radius) const {
    const ChunkCoord center = world_to_chunk(position);
    const int clamped_radius = std::clamp(max_radius, 0, chunk_radius_);

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

$WorldText = Replace-BetweenMarkers `
    $WorldText `
    "int WorldStreamer::continuous_uploaded_radius" `
    "void WorldStreamer::request_spawn_preload" `
    $ContinuousBlock `
    "WorldStreamer continuous uploaded radius cleanup" `
    ([ref]$WorldChanged) `
    $Log

# ----------------------------------------------------------------------
# 5. request_spawn_preload: ring-window is used only for initial safe loading.
# ----------------------------------------------------------------------

$SpawnPreloadBlock = @'
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

    int first_incomplete_ring = -1;
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
            first_incomplete_ring = ring;
            break;
        }
    }

    if (first_incomplete_ring < 0) {
        refresh_visible_chunks();
        flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
        return;
    }

    const int max_schedule_ring = std::min(
        preload_radius,
        first_incomplete_ring + contiguous_generation_ring_window() - 1
    );

    std::vector<ChunkCoord> ordered_chunks;
    for (int ring = first_incomplete_ring; ring <= max_schedule_ring; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
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

    if (requested > 0) {
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

$WorldText = Replace-BetweenMarkers `
    $WorldText `
    "void WorldStreamer::request_spawn_preload" `
    "void WorldStreamer::tick_generation_jobs" `
    $SpawnPreloadBlock `
    "WorldStreamer::request_spawn_preload initial ring-window only" `
    ([ref]$WorldChanged) `
    $Log

# ----------------------------------------------------------------------
# 6. refresh_visible_chunks: connected visible set in normal load area.
# ----------------------------------------------------------------------

$RefreshVisibleBlock = @'
void WorldStreamer::refresh_visible_chunks() {
    visible_chunks_.clear();

    const auto center_it = chunks_.find(observer_chunk_);
    if (center_it == chunks_.end() || !center_it->second.uploaded_to_gpu) {
        return;
    }

    const Vec3 stream_direction = normalized_horizontal_direction(observer_forward_);

    std::vector<ChunkCoord> open;
    std::vector<ChunkCoord> visited;
    open.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));
    visited.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));

    auto contains_coord = [](const std::vector<ChunkCoord>& list, ChunkCoord coord) {
        return std::find(list.begin(), list.end(), coord) != list.end();
    };

    open.push_back(observer_chunk_);
    visited.push_back(observer_chunk_);

    constexpr std::array<std::array<int, 2>, 4> neighbors {{
        {{ 1,  0}},
        {{-1,  0}},
        {{ 0,  1}},
        {{ 0, -1}}
    }};

    std::size_t cursor = 0;
    while (cursor < open.size()) {
        const ChunkCoord current = open[cursor++];

        const auto record_it = chunks_.find(current);
        if (record_it == chunks_.end() ||
            !record_it->second.uploaded_to_gpu ||
            !load_area_chunk(observer_chunk_, current, chunk_radius_)) {
            continue;
        }

        visible_chunks_.push_back({current});

        for (const auto& offset : neighbors) {
            const ChunkCoord next {current.x + offset[0], current.z + offset[1]};
            if (contains_coord(visited, next)) {
                continue;
            }

            if (!load_area_chunk(observer_chunk_, next, chunk_radius_)) {
                continue;
            }

            const auto next_it = chunks_.find(next);
            if (next_it == chunks_.end() || !next_it->second.uploaded_to_gpu) {
                continue;
            }

            visited.push_back(next);
            open.push_back(next);
        }
    }

    std::sort(
        visible_chunks_.begin(),
        visible_chunks_.end(),
        [&](const ActiveChunk& lhs, const ActiveChunk& rhs) {
            const float lhs_score = corridor_priority_score(
                observer_chunk_,
                lhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );
            const float rhs_score = corridor_priority_score(
                observer_chunk_,
                rhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );

            if (lhs_score != rhs_score) {
                return lhs_score < rhs_score;
            }

            if (lhs.coord.x != rhs.coord.x) {
                return lhs.coord.x < rhs.coord.x;
            }

            return lhs.coord.z < rhs.coord.z;
        }
    );
}

'@

$WorldText = Replace-BetweenMarkers `
    $WorldText `
    "void WorldStreamer::refresh_visible_chunks() {" `
    "std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads() {" `
    $RefreshVisibleBlock `
    "WorldStreamer::refresh_visible_chunks connected no-islands" `
    ([ref]$WorldChanged) `
    $Log

# ----------------------------------------------------------------------
# 7. pending uploads: priority selection and no erase(begin) loop.
# ----------------------------------------------------------------------

$DrainBudgetBlock = @'
std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads_by_budget(
    std::size_t byte_budget,
    std::size_t max_count,
    Vec3 observer_position,
    Vec3 observer_forward
) {
    std::vector<PendingChunkUpload> selected;
    if (max_count == 0 || pending_uploads_.empty()) {
        return selected;
    }

    const ChunkCoord origin = world_to_chunk(observer_position);
    const Vec3 stream_direction = normalized_horizontal_direction(observer_forward);

    const bool corridor_mode = corridor_mode_for_speed(observer_speed_blocks_per_second_);
    const std::size_t effective_max_count = corridor_mode
        ? std::min(max_count, corridor_uploads_per_frame())
        : max_count;

    const std::size_t effective_byte_budget = corridor_mode
        ? std::min(byte_budget, corridor_upload_byte_budget())
        : byte_budget;

    std::stable_sort(
        pending_uploads_.begin(),
        pending_uploads_.end(),
        [&](const PendingChunkUpload& lhs, const PendingChunkUpload& rhs) {
            const float lhs_score = corridor_priority_score(
                origin,
                lhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );
            const float rhs_score = corridor_priority_score(
                origin,
                rhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );

            if (lhs_score != rhs_score) {
                return lhs_score < rhs_score;
            }

            if (lhs.coord.x != rhs.coord.x) {
                return lhs.coord.x < rhs.coord.x;
            }

            return lhs.coord.z < rhs.coord.z;
        }
    );

    std::size_t used_bytes = 0;
    std::size_t take_count = 0;

    while (take_count < pending_uploads_.size() && take_count < effective_max_count) {
        const std::size_t upload_bytes = mesh_byte_count(pending_uploads_[take_count].mesh);
        if (take_count > 0 && used_bytes + upload_bytes > effective_byte_budget) {
            break;
        }

        used_bytes += upload_bytes;
        ++take_count;
    }

    selected.reserve(take_count);
    for (std::size_t index = 0; index < take_count; ++index) {
        selected.push_back(std::move(pending_uploads_[index]));
    }

    if (take_count > 0) {
        pending_uploads_.erase(
            pending_uploads_.begin(),
            pending_uploads_.begin() + static_cast<std::ptrdiff_t>(take_count)
        );
    }

    return selected;
}

'@

$WorldText = Replace-BetweenMarkers `
    $WorldText `
    "std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads_by_budget(" `
    "bool WorldStreamer::confirm_chunk_uploaded" `
    $DrainBudgetBlock `
    "WorldStreamer::drain_pending_uploads_by_budget no erase-begin loop" `
    ([ref]$WorldChanged) `
    $Log

if ($WorldChanged) {
    Write-Utf8NoBom $WorldStreamerCppPath $WorldText
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
}

# ----------------------------------------------------------------------
# 8. Renderer transition safety: keep menu buttons hidden during transition.
# ----------------------------------------------------------------------

$RendererText = Read-Utf8Text $RendererCppPath
$RendererChanged = $false

if ($RendererText.Contains("void Renderer::draw_menu_panorama_message(") -and
    -not $RendererText.Contains("menu_button_vertex_count_ = 0;")) {
    $RendererText = $RendererText.Replace(
        "const FrameResources& frame = frames_[current_frame_];",
        "menu_logo_vertex_count_ = 0;`r`n    menu_button_vertex_count_ = 0;`r`n    menu_button_highlight_vertex_count_ = 0;`r`n    menu_overlay_vertex_count_ = 0;`r`n    menu_text_vertex_count_ = 0;`r`n`r`n    const FrameResources& frame = frames_[current_frame_];"
    )
    $RendererChanged = $true
    Add-Log $Log "OK: renderer transition counters cleared"
}

if ($RendererChanged) {
    Write-Utf8NoBom $RendererCppPath $RendererText
    Add-Log $Log "OK: src/render/renderer.cpp written"
}

# ----------------------------------------------------------------------
# 9. Validation.
# ----------------------------------------------------------------------

$RuntimeAfter = Read-Utf8Text $RuntimeTuningPath
$WorldAfter = Read-Utf8Text $WorldStreamerCppPath
$RendererAfter = Read-Utf8Text $RendererCppPath

if ($WorldAfter.Contains("}float completed_result_apply_budget_ms")) {
    throw "Validation failed: broken helper separator still exists"
}

if (-not $RuntimeAfter.Contains("corridor_priority_only") -and -not $RuntimeAfter.Contains("priority only")) {
    Add-Log $Log "OK: priority-only mode described by field comments"
}

if (-not $WorldAfter.Contains("bool load_area_chunk(")) {
    throw "Validation failed: load_area_chunk helper missing"
}

if (-not $WorldAfter.Contains("bool keep_area_chunk(")) {
    throw "Validation failed: keep_area_chunk helper missing"
}

if (-not $WorldAfter.Contains("return load_area_chunk(origin, coord, chunk_radius);")) {
    throw "Validation failed: corridor_candidate_chunk is not load-area compatibility helper"
}

if (-not $WorldAfter.Contains("visible_chunks_.push_back({current});")) {
    throw "Validation failed: connected visible set missing"
}

if (-not $WorldAfter.Contains("std::size_t take_count = 0;")) {
    throw "Validation failed: pending upload range selection missing"
}

if ($WorldAfter.Contains("pending_uploads_.erase(pending_uploads_.begin());")) {
    throw "Validation failed: old erase(begin) loop still exists"
}

if ($WorldAfter.Contains("streaming_backlog_origin_.z != origin.z ||`r`n        corridor_mode") -or
    $WorldAfter.Contains("streaming_backlog_origin_.z != origin.z ||`n        corridor_mode")) {
    throw "Validation failed: backlog still rebuilds only because corridor_mode is active"
}

if (-not $WorldAfter.Contains("keep_area_chunk(origin, it->first, chunk_radius_)")) {
    throw "Validation failed: unload does not use keep area"
}

if ($RendererAfter.Contains("void Renderer::draw_menu_panorama_message(") -and
    -not $RendererAfter.Contains("menu_button_vertex_count_ = 0;")) {
    throw "Validation failed: transition menu button flicker fix missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/game/world_streamer.cpp"
if ($RendererChanged) {
    Add-Log $Log "- src/render/renderer.cpp"
}
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Cleaned WorldStreamer helper block and fixed the broken '}float' separator."
Add-Log $Log "- Kept corridor as priority only, not as the shape of the world."
Add-Log $Log "- Loading candidates still cover the normal chunk radius."
Add-Log $Log "- Unload uses keep radius to reduce popping on turns."
Add-Log $Log "- Gameplay visibility is connected inside normal load area, so islands are hidden without turning the world into a tunnel."
Add-Log $Log "- Spawn preload remains ring-window based, only for initial safe loading."
Add-Log $Log "- Pending uploads are selected by priority and erased by range, not erase(begin) in a loop."
Add-Log $Log "- Transition menu button counters remain cleared if the transition renderer exists."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Test standing/walking: world must look like a normal area, not a corridor."
Add-Log $Log "3. Test fast flight: forward chunks should have priority, but side chunks must still load."
Add-Log $Log "4. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
