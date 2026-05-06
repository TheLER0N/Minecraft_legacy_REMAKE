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

function Replace-FunctionBySignature(
    [string]$Text,
    [string]$Signature,
    [string]$Replacement,
    [string]$Name,
    [System.Collections.Generic.List[string]]$Log
) {
    $start = $Text.IndexOf($Signature)
    if ($start -lt 0) {
        throw "Cannot replace $Name`: signature not found"
    }

    $open = $Text.IndexOf("{", $start)
    if ($open -lt 0) {
        throw "Cannot replace $Name`: opening brace not found"
    }

    $close = Find-MatchingBrace $Text $open
    $end = $close + 1
    while ($end -lt $Text.Length -and ($Text[$end] -eq "`r" -or $Text[$end] -eq "`n")) {
        ++$end
    }

    Add-Log $Log ("OK: replaced full function " + $Name)
    return $Text.Substring(0, $start) + $Replacement + $Text.Substring($end)
}

$Log = New-Object System.Collections.Generic.List[string]
Add-Log $Log "Patch report"
Add-Log $Log ("Project root: " + $ProjectRoot)
Add-Log $Log ("Backup dir: " + $BackupDir)
Add-Log $Log ("Time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Log $Log ""

$WorldStreamerPath = Join-Path $ProjectRoot "src\game\world_streamer.cpp"

if (-not (Test-Path $WorldStreamerPath)) {
    throw "Required file not found: $WorldStreamerPath"
}

Backup-File $WorldStreamerPath

$text = Read-Utf8Text $WorldStreamerPath
$original = $text

# ----------------------------------------------------------------------
# 1. Add bucket helpers before kMaxDirtyChunkSavesPerTick.
#    No graphics, textures, lighting, render distance, or mesh code is changed.
# ----------------------------------------------------------------------

if (-not $text.Contains("int chunk_streaming_bucket(")) {
    $helpers = @'

int chunk_streaming_bucket(
    ChunkCoord origin,
    ChunkCoord coord,
    Vec3 direction,
    float speed_blocks_per_second
) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    const int chebyshev = std::max(std::abs(dx), std::abs(dz));

    if (chebyshev == 0) {
        return 0;
    }

    if (chebyshev <= corridor_safe_radius_chunks()) {
        return 0;
    }

    if (chebyshev <= corridor_safe_radius_chunks() + 2) {
        return 1;
    }

    const float forward = chunk_forward_units(origin, coord, direction);
    const float side = chunk_side_units(origin, coord, direction);

    if (corridor_mode_for_speed(speed_blocks_per_second)) {
        const int forward_limit = adaptive_corridor_forward_chunks(speed_blocks_per_second, target_chunk_radius());
        if (forward >= 0.0f &&
            forward <= static_cast<float>(forward_limit) &&
            side <= static_cast<float>(corridor_outer_half_width_chunks())) {
            return 2;
        }
    } else if (forward >= 0.0f && side <= static_cast<float>(corridor_outer_half_width_chunks())) {
        return 2;
    }

    if (side <= static_cast<float>(corridor_outer_half_width_chunks() + 2)) {
        return 3;
    }

    return 4;
}

void reorder_chunk_coords_by_buckets(
    std::vector<ChunkCoord>& coords,
    ChunkCoord origin,
    Vec3 direction,
    float speed_blocks_per_second
) {
    if (coords.size() < 2) {
        return;
    }

    std::array<std::vector<ChunkCoord>, 5> buckets {};
    for (std::vector<ChunkCoord>& bucket : buckets) {
        bucket.reserve(coords.size() / buckets.size() + 1);
    }

    for (const ChunkCoord& coord : coords) {
        const int bucket = std::clamp(
            chunk_streaming_bucket(origin, coord, direction, speed_blocks_per_second),
            0,
            static_cast<int>(buckets.size()) - 1
        );
        buckets[static_cast<std::size_t>(bucket)].push_back(coord);
    }

    coords.clear();
    for (std::vector<ChunkCoord>& bucket : buckets) {
        for (const ChunkCoord& coord : bucket) {
            coords.push_back(coord);
        }
    }
}

'@

    $marker = "constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;"
    if (-not $text.Contains($marker)) {
        throw "Cannot insert bucket helpers: kMaxDirtyChunkSavesPerTick marker not found"
    }

    $text = $text.Replace($marker, $helpers + $marker)
    Add-Log $Log "OK: inserted chunk bucket helpers"
} else {
    Add-Log $Log "SKIP: chunk bucket helpers already exist"
}

# ----------------------------------------------------------------------
# 2. Replace full update_observer function.
#    This avoids brittle partial replacements.
# ----------------------------------------------------------------------

$UpdateObserverFunction = @'
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

        reorder_chunk_coords_by_buckets(
            streaming_backlog_,
            origin,
            stream_direction,
            observer_speed_blocks_per_second_
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

        std::array<std::vector<ChunkJob>, 5> job_buckets {};
        for (std::vector<ChunkJob>& bucket : job_buckets) {
            bucket.reserve(job_queue_.size() / job_buckets.size() + 1);
        }

        for (ChunkJob& job : job_queue_) {
            const int bucket = std::clamp(
                chunk_streaming_bucket(
                    origin,
                    job.coord,
                    stream_direction,
                    observer_speed_blocks_per_second_
                ),
                0,
                static_cast<int>(job_buckets.size()) - 1
            );
            job_buckets[static_cast<std::size_t>(bucket)].push_back(std::move(job));
        }

        job_queue_.clear();
        for (std::vector<ChunkJob>& bucket : job_buckets) {
            for (ChunkJob& job : bucket) {
                job_queue_.push_back(std::move(job));
            }
        }
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

$text = Replace-FunctionBySignature `
    $text `
    "void WorldStreamer::update_observer(Vec3 position, Vec3 forward, float dt_seconds)" `
    $UpdateObserverFunction `
    "WorldStreamer::update_observer bucketed" `
    $Log

# ----------------------------------------------------------------------
# 3. Replace full request_spawn_preload function.
#    ordered_chunks stable_sort remains, because spawn preload must be contiguous.
# ----------------------------------------------------------------------

$SpawnPreloadFunction = @'
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

        std::array<std::vector<ChunkJob>, 5> job_buckets {};
        const Vec3 preload_direction = normalized_horizontal_direction(observer_forward_);
        for (std::vector<ChunkJob>& bucket : job_buckets) {
            bucket.reserve(job_queue_.size() / job_buckets.size() + 1);
        }

        for (ChunkJob& job : job_queue_) {
            const int bucket = std::clamp(
                chunk_streaming_bucket(center, job.coord, preload_direction, 0.0f),
                0,
                static_cast<int>(job_buckets.size()) - 1
            );
            job_buckets[static_cast<std::size_t>(bucket)].push_back(std::move(job));
        }

        job_queue_.clear();
        for (std::vector<ChunkJob>& bucket : job_buckets) {
            for (ChunkJob& job : bucket) {
                job_queue_.push_back(std::move(job));
            }
        }
    }

    refresh_visible_chunks();
    flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
}

'@

$text = Replace-FunctionBySignature `
    $text `
    "void WorldStreamer::request_spawn_preload" `
    $SpawnPreloadFunction `
    "WorldStreamer::request_spawn_preload bucketed" `
    $Log

# ----------------------------------------------------------------------
# 4. Validation: no fragile global failure on unrelated text.
# ----------------------------------------------------------------------

if (-not $text.Contains("int chunk_streaming_bucket(")) {
    throw "Validation failed: chunk_streaming_bucket helper missing"
}

if (-not $text.Contains("void reorder_chunk_coords_by_buckets(")) {
    throw "Validation failed: reorder_chunk_coords_by_buckets helper missing"
}

if (-not $text.Contains("reorder_chunk_coords_by_buckets(`r`n            streaming_backlog_,") -and
    -not $text.Contains("reorder_chunk_coords_by_buckets(`n            streaming_backlog_,")) {
    throw "Validation failed: update_observer does not bucket streaming_backlog_"
}

if (-not $text.Contains("std::array<std::vector<ChunkJob>, 5> job_buckets")) {
    throw "Validation failed: job bucket code missing"
}

if (-not $text.Contains("chunk_streaming_bucket(center, job.coord, preload_direction, 0.0f)")) {
    throw "Validation failed: spawn preload job bucket code missing"
}

if (-not [regex]::IsMatch($text, "std::stable_sort\s*\(\s*ordered_chunks\.begin\s*\(")) {
    throw "Validation failed: ordered_chunks ring sort was removed"
}

if (-not $text.Contains("return load_area_chunk(origin, coord, chunk_radius);")) {
    throw "Validation failed: corridor_candidate_chunk is no longer priority-only compatibility helper"
}

if (-not $text.Contains("constexpr int kMaxChunkRadius = 24;")) {
    throw "Validation failed: desktop kMaxChunkRadius cap changed"
}

if (-not $text.Contains("std::unordered_set<ChunkCoord, ChunkCoordHasher> visited;")) {
    throw "Validation failed: refresh_visible_chunks unordered_set optimization missing"
}

if ($text -eq $original) {
    Add-Log $Log "No changes were needed."
} else {
    Write-Utf8NoBom $WorldStreamerPath $text
    Add-Log $Log "OK: src/game/world_streamer.cpp written"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_streamer.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Added coarse chunk priority buckets."
Add-Log $Log "- Rewrote update_observer as a full function to avoid fragile partial replacements."
Add-Log $Log "- Replaced streaming_backlog_ sort with bucket reorder."
Add-Log $Log "- Replaced update_observer job_queue_ stable_sort with bucket move."
Add-Log $Log "- Rewrote request_spawn_preload as a full function."
Add-Log $Log "- Kept ordered_chunks ring sort for safe spawn preload."
Add-Log $Log "- Did not change graphics, textures, shaders, lighting, block geometry, render distance, or mesh generation."
Add-Log $Log ""
Add-Log $Log "Expected result:"
Add-Log $Log "- Lower CPU spikes during movement and chunk generation."
Add-Log $Log "- Less micro-freeze from sorting large queues."
Add-Log $Log "- Chunk loading still prioritizes player/safety/forward areas."
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
