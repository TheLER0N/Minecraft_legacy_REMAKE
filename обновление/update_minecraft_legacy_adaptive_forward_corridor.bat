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

$WorldStreamerCpp = Join-Path $ProjectRoot "src\game\world_streamer.cpp"
$WorldStreamerHpp = Join-Path $ProjectRoot "src\game\world_streamer.hpp"
$RuntimeTuning = Join-Path $ProjectRoot "src\game\world_runtime_tuning.hpp"
$Application = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @($WorldStreamerCpp, $WorldStreamerHpp, $Application)
foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $WorldStreamerCpp
Backup-File $WorldStreamerHpp
Backup-File $Application
if (Test-Path $RuntimeTuning) {
    Backup-File $RuntimeTuning
}

# ----------------------------------------------------------------------
# 1. Runtime tuning target: adaptive forward corridor.
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
        35.0f
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
        45.0f
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuning $runtimeTuningText
Add-Log $Log "OK: runtime tuning rewritten"
Add-Log $Log "    - min forward buffer = 3 chunks"
Add-Log $Log "    - max forward buffer = 8 chunks"
Add-Log $Log "    - forward corridor width = 3..7 chunks"
Add-Log $Log "    - speed thresholds = 25/45 blocks per second"

# ----------------------------------------------------------------------
# 2. world_streamer.hpp: dt overload and speed state.
# ----------------------------------------------------------------------

$hpp = Read-Utf8Text $WorldStreamerHpp
$hppChanged = $false

if (-not $hpp.Contains("void update_observer(Vec3 position, Vec3 forward, float dt_seconds);")) {
    $oldDecl = '    void update_observer(Vec3 position, Vec3 forward);'
    $newDecl = @'
    void update_observer(Vec3 position, Vec3 forward);
    void update_observer(Vec3 position, Vec3 forward, float dt_seconds);
'@

    if ($hpp.Contains($oldDecl)) {
        $hpp = $hpp.Replace($oldDecl, $newDecl.TrimEnd())
        $hppChanged = $true
        Add-Log $Log "OK: added update_observer(Vec3, Vec3, float) declaration"
    } else {
        throw "Cannot patch world_streamer.hpp: update_observer declaration anchor not found"
    }
} else {
    Add-Log $Log "OK: dt overload declaration already exists"
}

if (-not $hpp.Contains("observer_speed_blocks_per_second_")) {
    $oldState = @'
    Vec3 last_streaming_update_position_ {};
    bool has_streaming_update_position_ {false};
'@
    $newState = @'
    Vec3 last_streaming_update_position_ {};
    bool has_streaming_update_position_ {false};
    Vec3 previous_observer_position_ {};
    bool has_previous_observer_position_ {false};
    float observer_speed_blocks_per_second_ {0.0f};
'@

    if ($hpp.Contains($oldState)) {
        $hpp = $hpp.Replace($oldState, $newState)
        $hppChanged = $true
        Add-Log $Log "OK: added observer speed tracking fields"
    } else {
        throw "Cannot patch world_streamer.hpp: streaming state anchor not found"
    }
} else {
    Add-Log $Log "OK: observer speed state already exists"
}

if ($hppChanged) {
    Write-Utf8NoBom $WorldStreamerHpp $hpp
    Add-Log $Log "OK: src/game/world_streamer.hpp written"
}

# ----------------------------------------------------------------------
# 3. world_streamer.cpp: helpers, observer update and priority function.
# ----------------------------------------------------------------------

$cpp = Read-Utf8Text $WorldStreamerCpp
$cppChanged = $false

if (-not $cpp.Contains("#include <cmath>")) {
    if ($cpp.Contains("#include <chrono>")) {
        $cpp = $cpp.Replace("#include <chrono>", "#include <chrono>`r`n#include <cmath>")
        $cppChanged = $true
        Add-Log $Log "OK: added <cmath> include"
    } else {
        throw "Cannot add <cmath>: <chrono> anchor not found"
    }
} else {
    Add-Log $Log "OK: <cmath> include already exists"
}

if ($cpp.Contains("}constexpr std::size_t kMaxDirtyChunkSavesPerTick")) {
    $cpp = $cpp.Replace(
        "}constexpr std::size_t kMaxDirtyChunkSavesPerTick",
        "}`r`n`r`nconstexpr std::size_t kMaxDirtyChunkSavesPerTick"
    )
    $cppChanged = $true
    Add-Log $Log "OK: fixed '}constexpr' formatting defect"
}

$helperPattern = '(?s)std::size_t\s+max_new_chunk_requests_per_frame\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.max_new_chunk_requests_per_frame;\s*\}\s*std::size_t\s+max_completed_results_per_tick\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.max_completed_results_per_tick;\s*\}\s*std::size_t\s+max_job_queue_size\(\)\s*\{\s*return\s+world_runtime_tuning\(\)\.max_job_queue_size;\s*\}(?:\s*float\s+completed_result_apply_budget_ms\(\)\s*\{.*?\})?(?:\s*int\s+target_chunk_radius\(\)\s*\{.*?\})?(?:\s*float\s+streaming_update_distance_blocks\(\)\s*\{.*?\})?(?:\s*float\s+forward_priority_weight\(\)\s*\{.*?\})?(?:\s*float\s+side_priority_weight\(\)\s*\{.*?\})?(?:\s*float\s+back_priority_penalty\(\)\s*\{.*?\})?(?:\s*int\s+min_forward_buffer_chunks\(\)\s*\{.*?\})?(?:\s*int\s+max_forward_buffer_chunks\(\)\s*\{.*?\})?(?:\s*int\s+min_forward_width_chunks\(\)\s*\{.*?\})?(?:\s*int\s+max_forward_width_chunks\(\)\s*\{.*?\})?(?:\s*float\s+forward_buffer_pipeline_seconds\(\)\s*\{.*?\})?(?:\s*float\s+forward_buffer_safety_blocks\(\)\s*\{.*?\})?(?:\s*float\s+fast_flight_speed_threshold\(\)\s*\{.*?\})?(?:\s*float\s+very_fast_flight_speed_threshold\(\)\s*\{.*?\})?(?:\s*int\s+adaptive_forward_buffer_chunks\(float\s+speed_blocks_per_second\)\s*\{.*?\})?(?:\s*int\s+adaptive_forward_width_chunks\(float\s+speed_blocks_per_second\)\s*\{.*?\})?'
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
'@

if ([regex]::IsMatch($cpp, $helperPattern)) {
    $cpp = [regex]::Replace(
        $cpp,
        $helperPattern,
        [System.Text.RegularExpressions.MatchEvaluator]{ param($m) $helperBlock },
        1
    )
    $cppChanged = $true
    Add-Log $Log "OK: normalized runtime helper block with adaptive forward corridor helpers"
} else {
    throw "Cannot normalize runtime helpers: helper block not found"
}

if (-not $cpp.Contains("std::max(chunk_radius, target_chunk_radius())")) {
    $constructorOld = ', chunk_radius_(std::clamp(chunk_radius, kMinChunkRadius, kMaxChunkRadius)) {'
    $constructorNew = ', chunk_radius_(std::clamp(std::max(chunk_radius, target_chunk_radius()), kMinChunkRadius, kMaxChunkRadius)) {'

    if ($cpp.Contains($constructorOld)) {
        $cpp = $cpp.Replace($constructorOld, $constructorNew)
        $cppChanged = $true
        Add-Log $Log "OK: constructor now enforces target chunk radius"
    } else {
        Add-Log $Log "OK: constructor target radius enforcement already present or anchor changed"
    }
} else {
    Add-Log $Log "OK: constructor target radius enforcement already exists"
}

# Replace small wrappers and full update_observer block.
$updateObserverReplacement = @'
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

    const float streaming_distance = streaming_update_distance_blocks();
    const float move_dx = position.x - last_streaming_update_position_.x;
    const float move_dz = position.z - last_streaming_update_position_.z;
    const float move_distance_sq = move_dx * move_dx + move_dz * move_dz;
    const float required_distance_sq = streaming_distance * streaming_distance;

    if (has_streaming_update_position_ && move_distance_sq < required_distance_sq) {
        refresh_visible_chunks();
        flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
        return;
    }

    last_streaming_update_position_ = position;
    has_streaming_update_position_ = true;

    std::vector<ChunkCoord> missing_chunks;
    missing_chunks.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));

    for (int dz = -chunk_radius_; dz <= chunk_radius_; ++dz) {
        for (int dx = -chunk_radius_; dx <= chunk_radius_; ++dx) {
            const ChunkCoord coord {origin.x + dx, origin.z + dz};

            if (auto it = chunks_.find(coord); it != chunks_.end()) {
                it->second.last_touched_frame = frame_counter_;
                continue;
            }

            missing_chunks.push_back(coord);
        }
    }

    std::sort(
        missing_chunks.begin(),
        missing_chunks.end(),
        [&](const ChunkCoord& lhs, const ChunkCoord& rhs) {
            const float lhs_score = chunk_priority_score(lhs, observer_position_, observer_forward_);
            const float rhs_score = chunk_priority_score(rhs, observer_position_, observer_forward_);

            if (lhs_score != rhs_score) {
                return lhs_score < rhs_score;
            }

            if (lhs.x != rhs.x) {
                return lhs.x < rhs.x;
            }

            return lhs.z < rhs.z;
        }
    );

    std::size_t requested_this_frame = 0;

    for (const ChunkCoord& coord : missing_chunks) {
        if (requested_this_frame >= max_new_chunk_requests_per_frame()) {
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
        ++requested_this_frame;
    }

    {
        std::lock_guard lock(mutex_);
        std::stable_sort(
            job_queue_.begin(),
            job_queue_.end(),
            [&](const ChunkJob& lhs, const ChunkJob& rhs) {
                return job_priority_score_locked(lhs) < job_priority_score_locked(rhs);
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

$cpp = Replace-BetweenMarkers `
    $cpp `
    "void WorldStreamer::update_observer(Vec3 position) {" `
    "void WorldStreamer::tick_generation_jobs() {" `
    $updateObserverReplacement `
    "WorldStreamer::update_observer overloads" `
    ([ref]$cppChanged) `
    $Log

$priorityReplacement = @'
float WorldStreamer::chunk_priority_score(ChunkCoord coord, Vec3 observer_position, Vec3 observer_forward) const {
    const ChunkCoord origin = world_to_chunk(observer_position);
    const float dx = static_cast<float>(coord.x - origin.x);
    const float dz = static_cast<float>(coord.z - origin.z);

    Vec3 forward = normalize({observer_forward.x, 0.0f, observer_forward.z});
    if (length(forward) <= 0.00001f) {
        forward = {0.0f, 0.0f, -1.0f};
    }

    const float right_x = -forward.z;
    const float right_z = forward.x;

    const float forward_axis = dx * forward.x + dz * forward.z;
    const float side_axis = dx * right_x + dz * right_z;
    const float side_abs = std::abs(side_axis);
    const float dist_sq = dx * dx + dz * dz;

    const int forward_depth = adaptive_forward_buffer_chunks(observer_speed_blocks_per_second_);
    const int forward_width = adaptive_forward_width_chunks(observer_speed_blocks_per_second_);
    const float half_width = static_cast<float>(forward_width) * 0.5f;

    const bool hot_zone = dist_sq <= 4.0f;
    const bool in_forward_corridor =
        forward_axis > 0.25f &&
        forward_axis <= static_cast<float>(forward_depth) + 0.5f &&
        side_abs <= half_width;

    if (hot_zone) {
        return -30000.0f + dist_sq + side_abs * 0.5f;
    }

    if (in_forward_corridor) {
        const float front_edge_distance = std::abs(static_cast<float>(forward_depth) - forward_axis);
        return
            -20000.0f +
            front_edge_distance * 12.0f +
            side_abs * 5.0f -
            forward_axis * 2.0f +
            dist_sq * 0.01f;
    }

    float score =
        dist_sq * 0.35f -
        forward_axis * forward_priority_weight() +
        side_abs * side_priority_weight();

    if (forward_axis < -0.25f) {
        score += back_priority_penalty() + std::abs(forward_axis) * forward_priority_weight();
    }

    return score;
}

'@

$priorityStart = "float WorldStreamer::chunk_priority_score(ChunkCoord coord, Vec3 observer_position, Vec3 observer_forward) const {"
$priorityEnd = "float WorldStreamer::job_priority_score_locked"
if ($cpp.Contains($priorityStart) -and $cpp.Contains($priorityEnd)) {
    $cpp = Replace-BetweenMarkers `
        $cpp `
        $priorityStart `
        $priorityEnd `
        $priorityReplacement `
        "WorldStreamer::chunk_priority_score" `
        ([ref]$cppChanged) `
        $Log
} else {
    throw "Cannot replace chunk_priority_score: function markers not found"
}

if ($cppChanged) {
    Write-Utf8NoBom $WorldStreamerCpp $cpp
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
}

# ----------------------------------------------------------------------
# 4. application.cpp: pass dt into streamer.
# ----------------------------------------------------------------------

$app = Read-Utf8Text $Application
$appChanged = $false

if ($app.Contains("world_streamer_->update_observer(observer_position, observer_forward);")) {
    $app = $app.Replace(
        "world_streamer_->update_observer(observer_position, observer_forward);",
        "world_streamer_->update_observer(observer_position, observer_forward, dt);"
    )
    $appChanged = $true
    Add-Log $Log "OK: application.cpp passes dt into update_observer"
} elseif ($app.Contains("world_streamer_->update_observer(observer_position, observer_forward, dt);")) {
    Add-Log $Log "OK: application.cpp already passes dt"
} else {
    throw "Cannot patch application.cpp: update_observer call not found"
}

if ($app.Contains("constexpr int kInitialChunkRadius = 10;")) {
    $app = $app.Replace("constexpr int kInitialChunkRadius = 10;", "constexpr int kInitialChunkRadius = 16;")
    $appChanged = $true
    Add-Log $Log "OK: application.cpp desktop kInitialChunkRadius changed to 16"
} elseif ($app.Contains("constexpr int kInitialChunkRadius = 16;")) {
    Add-Log $Log "OK: application.cpp desktop kInitialChunkRadius already 16"
} else {
    Add-Log $Log "WARN: application.cpp exact desktop kInitialChunkRadius not found"
}

if ($appChanged) {
    Write-Utf8NoBom $Application $app
}

# ----------------------------------------------------------------------
# 5. Validation.
# ----------------------------------------------------------------------

$rtAfter = Read-Utf8Text $RuntimeTuning
$hppAfter = Read-Utf8Text $WorldStreamerHpp
$cppAfter = Read-Utf8Text $WorldStreamerCpp
$appAfter = Read-Utf8Text $Application

if (-not $rtAfter.Contains("int min_forward_buffer_chunks {3};")) {
    throw "Validation failed: min_forward_buffer_chunks missing"
}

if (-not $rtAfter.Contains("int max_forward_buffer_chunks {8};")) {
    throw "Validation failed: max_forward_buffer_chunks missing"
}

if (-not $hppAfter.Contains("void update_observer(Vec3 position, Vec3 forward, float dt_seconds);")) {
    throw "Validation failed: update_observer dt overload declaration missing"
}

if (-not $hppAfter.Contains("observer_speed_blocks_per_second_")) {
    throw "Validation failed: observer speed field missing"
}

if (-not $cppAfter.Contains("void WorldStreamer::update_observer(Vec3 position, Vec3 forward, float dt_seconds)")) {
    throw "Validation failed: update_observer dt overload implementation missing"
}

if (-not $cppAfter.Contains("adaptive_forward_buffer_chunks(observer_speed_blocks_per_second_)")) {
    throw "Validation failed: adaptive forward buffer is not used in priority"
}

if (-not $cppAfter.Contains("in_forward_corridor")) {
    throw "Validation failed: forward corridor priority missing"
}

if (-not $appAfter.Contains("world_streamer_->update_observer(observer_position, observer_forward, dt);")) {
    throw "Validation failed: application does not pass dt"
}

if (-not $cppAfter.Contains("void WorldStreamer::tick_generation_jobs()")) {
    throw "Validation failed: tick_generation_jobs marker missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/game/world_streamer.hpp"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Added adaptive forward corridor instead of one forward chunk."
Add-Log $Log "- Minimum forward buffer is 3 chunks."
Add-Log $Log "- Fast movement raises forward buffer up to 8 chunks."
Add-Log $Log "- Corridor width grows from 3 to 7 chunks."
Add-Log $Log "- Application now passes frame dt so WorldStreamer can estimate movement speed."
Add-Log $Log "- Existing radius 16 and 4-block streaming gate remain."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Test walking, fast movement and debug flying."
Add-Log $Log "3. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
