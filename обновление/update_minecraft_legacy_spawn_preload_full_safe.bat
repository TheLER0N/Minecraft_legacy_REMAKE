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

$ApplicationHpp = Join-Path $ProjectRoot "src\app\application.hpp"
$ApplicationCpp = Join-Path $ProjectRoot "src\app\application.cpp"
$RuntimeTuning = Join-Path $ProjectRoot "src\game\world_runtime_tuning.hpp"

$RequiredFiles = @($ApplicationHpp, $ApplicationCpp, $RuntimeTuning)
foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $ApplicationHpp
Backup-File $ApplicationCpp
Backup-File $RuntimeTuning

# ----------------------------------------------------------------------
# 1. Runtime tuning target: keep all optimization fields and add spawn preload.
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
    std::size_t spawn_preload_min_visible_chunks {9};
    int spawn_preload_max_frames {900};
    std::size_t spawn_preload_upload_max_count {16};
    int transition_black_frames {2};
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
        std::size_t {4},
        600,
        std::size_t {8},
        2
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
        std::size_t {9},
        900,
        std::size_t {16},
        2
    };
#endif
}

} // namespace ml
'@

Write-Utf8NoBom $RuntimeTuning $runtimeTuningText
Add-Log $Log "OK: src/game/world_runtime_tuning.hpp rewritten"
Add-Log $Log "    - keeps 16 chunk target"
Add-Log $Log "    - keeps adaptive forward corridor settings"
Add-Log $Log "    - adds spawn preload settings"

# ----------------------------------------------------------------------
# 2. application.hpp: add helper declarations.
# ----------------------------------------------------------------------

$hpp = Read-Utf8Text $ApplicationHpp
$hppChanged = $false

if (-not $hpp.Contains("bool preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward);")) {
    $oldDeclBlock = @'
    void start_world();
    Renderer::CaveVisibilityFrame update_cave_visibility_frame(Vec3 observer_position);
'@
    $newDeclBlock = @'
    void start_world();
    bool preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward);
    void render_black_transition_frames(int frame_count);
    void unload_world_for_menu();
    Renderer::CaveVisibilityFrame update_cave_visibility_frame(Vec3 observer_position);
'@

    if ($hpp.Contains($oldDeclBlock)) {
        $hpp = $hpp.Replace($oldDeclBlock, $newDeclBlock)
        $hppChanged = $true
        Add-Log $Log "OK: application.hpp added preload/transition/unload helper declarations"
    } else {
        throw "Cannot patch application.hpp: helper declaration anchor not found"
    }
} else {
    Add-Log $Log "OK: application.hpp helper declarations already exist"
}

if ($hppChanged) {
    Write-Utf8NoBom $ApplicationHpp $hpp
    Add-Log $Log "OK: src/app/application.hpp written"
}

# ----------------------------------------------------------------------
# 3. application.cpp: replace start_world block with preload helpers.
# ----------------------------------------------------------------------

$app = Read-Utf8Text $ApplicationCpp
$appChanged = $false

$startAndHelperBlock = @'
void Application::start_world() {
    render_black_transition_frames(world_runtime_tuning().transition_black_frames);

    if (world_streamer_ == nullptr) {
        try {
            const std::filesystem::path save_root = platform_.save_root_directory() / "default";
            log_message(LogLevel::Info, "Application: start_world save_root='" + path_to_utf8(save_root) + "'");

            world_save_ = std::make_unique<WorldSave>(save_root);
            const WorldMetadata metadata = world_save_->load_or_create_metadata();
            log_message(LogLevel::Info, "Application: world metadata loaded seed=" + std::to_string(metadata.world_seed));

            world_streamer_ = std::make_unique<WorldStreamer>(metadata.world_seed, block_registry_, kInitialChunkRadius, world_save_.get());
            world_streamer_->set_leaves_render_mode(leaves_render_mode_);

            const WorldGenerator spawn_generator {block_registry_};
            const int spawn_x = 32;
            const int spawn_z = 80;
            const int surface_y = spawn_generator.surface_height_at(spawn_x, spawn_z, metadata.world_seed);
            const float spawn_y = static_cast<float>(std::max(surface_y, kSeaLevel) + 2);

            player_.set_body_position({static_cast<float>(spawn_x), spawn_y, static_cast<float>(spawn_z)});
            camera_.set_pose({static_cast<float>(spawn_x), spawn_y + kPlayerEyeHeight, static_cast<float>(spawn_z)}, -90.0f, -22.0f);
            player_.set_view_from_forward(camera_.forward());

            if (!preload_world_spawn(player_.position(), player_.forward())) {
                world_streamer_.reset();
                world_save_.reset();
                platform_.set_mouse_capture(false);
                app_state_ = AppState::MainMenu;
                return;
            }

            log_message(LogLevel::Info, "Application: world streamer created seed=" + std::to_string(metadata.world_seed));
        } catch (const std::exception& exception) {
            log_message(LogLevel::Error, std::string("Application: failed to start world: ") + exception.what());
            world_streamer_.reset();
            world_save_.reset();
            platform_.set_mouse_capture(false);
            app_state_ = AppState::MainMenu;
            return;
        } catch (...) {
            log_message(LogLevel::Error, "Application: failed to start world: unknown exception");
            world_streamer_.reset();
            world_save_.reset();
            platform_.set_mouse_capture(false);
            app_state_ = AppState::MainMenu;
            return;
        }
    }

    platform_.set_mouse_capture(true);
    platform_.enter_world_music();
    app_state_ = AppState::InWorld;
}

void Application::render_black_transition_frames(int frame_count) {
    const CameraFrameData loading_camera {
        Mat4::identity(),
        Mat4::identity(),
        Mat4::identity(),
        {},
        {0.0f, 0.0f, -1.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}
    };

    for (int i = 0; i < frame_count && !platform_.should_close(); ++i) {
        platform_.pump_events();
        renderer_.begin_frame(loading_camera);
        renderer_.end_frame();
    }
}

bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward) {
    if (world_streamer_ == nullptr) {
        return false;
    }

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
    const std::size_t min_visible_chunks = world_runtime_tuning().spawn_preload_min_visible_chunks;
    const std::size_t upload_max_count = world_runtime_tuning().spawn_preload_upload_max_count;
    bool spawn_column_loaded_once = false;

    log_message(
        LogLevel::Info,
        std::string("Application: preload spawn chunks begin [min_visible=") +
            std::to_string(min_visible_chunks) +
            ", max_frames=" + std::to_string(max_frames) + "]"
    );

    for (int frame = 0; frame < max_frames && !platform_.should_close(); ++frame) {
        platform_.pump_events();

        world_streamer_->update_observer(spawn_position, spawn_forward);
        world_streamer_->tick_generation_jobs();

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

void Application::unload_world_for_menu() {
    render_black_transition_frames(world_runtime_tuning().transition_black_frames);

    if (world_streamer_ != nullptr) {
        world_streamer_->flush_all_dirty_chunks();

        for (const ActiveChunk& chunk : world_streamer_->visible_chunks()) {
            renderer_.unload_chunk_mesh(chunk.coord);
        }

        for (const ChunkCoord& coord : world_streamer_->drain_pending_unloads()) {
            renderer_.unload_chunk_mesh(coord);
        }

        world_streamer_.reset();
        world_save_.reset();
    }

    hovered_block_.reset();
    block_break_.target.reset();
    block_break_.repeat_seconds = 0.0f;

    render_black_transition_frames(world_runtime_tuning().transition_black_frames);
}

'@

$app = Replace-BetweenMarkers `
    $app `
    "void Application::start_world() {" `
    "Renderer::CaveVisibilityFrame Application::update_cave_visibility_frame" `
    $startAndHelperBlock `
    "Application::start_world/preload/unload helper block" `
    ([ref]$appChanged) `
    $Log

# Patch PauseMenu exit branch robustly after replacing start_world block.
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
$oldExitBlock = $app.Substring($exitStart, $exitEnd - $exitStart)

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

if (-not $oldExitBlock.Contains("unload_world_for_menu();")) {
    $app = $app.Substring(0, $exitStart) + $newExitBlock + $app.Substring($exitEnd)
    $appChanged = $true
    Add-Log $Log "OK: PauseMenu Exit branch now unloads world through unload_world_for_menu()"
} else {
    Add-Log $Log "OK: PauseMenu Exit branch already uses unload_world_for_menu()"
}

if ($appChanged) {
    Write-Utf8NoBom $ApplicationCpp $app
    Add-Log $Log "OK: src/app/application.cpp written"
}

# ----------------------------------------------------------------------
# 4. Validation.
# ----------------------------------------------------------------------

$rtAfter = Read-Utf8Text $RuntimeTuning
$hppAfter = Read-Utf8Text $ApplicationHpp
$appAfter = Read-Utf8Text $ApplicationCpp

if (-not $rtAfter.Contains("spawn_preload_min_visible_chunks")) {
    throw "Validation failed: spawn preload tuning fields missing"
}

if (-not $hppAfter.Contains("bool preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward);")) {
    throw "Validation failed: preload_world_spawn declaration missing"
}

if (-not $hppAfter.Contains("void render_black_transition_frames(int frame_count);")) {
    throw "Validation failed: render_black_transition_frames declaration missing"
}

if (-not $hppAfter.Contains("void unload_world_for_menu();")) {
    throw "Validation failed: unload_world_for_menu declaration missing"
}

if (-not $appAfter.Contains("bool Application::preload_world_spawn(Vec3 spawn_position, Vec3 spawn_forward)")) {
    throw "Validation failed: preload_world_spawn implementation missing"
}

if (-not $appAfter.Contains("void Application::render_black_transition_frames(int frame_count)")) {
    throw "Validation failed: render_black_transition_frames implementation missing"
}

if (-not $appAfter.Contains("void Application::unload_world_for_menu()")) {
    throw "Validation failed: unload_world_for_menu implementation missing"
}

if (-not $appAfter.Contains("loaded_probe_columns >= 5")) {
    throw "Validation failed: spawn probe loading check missing"
}

if (-not $appAfter.Contains("renderer_.upload_chunk_mesh(upload.coord, upload.mesh, upload.visibility)")) {
    throw "Validation failed: preload upload path missing"
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
Add-Log $Log "- src/app/application.hpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Adds transition frames before entering and leaving the world."
Add-Log $Log "- Adds spawn preload before AppState::InWorld."
Add-Log $Log "- Forces spawn chunk/near columns to load before player control."
Add-Log $Log "- Uploads spawn-area chunk meshes before entering the world."
Add-Log $Log "- PauseMenu Exit now unloads world meshes and resets world_streamer/world_save."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Start a world and check that the player does not fall before chunks appear."
Add-Log $Log "3. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
