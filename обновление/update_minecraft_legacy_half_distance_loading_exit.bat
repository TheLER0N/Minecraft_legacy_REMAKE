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
# 1. Runtime tuning:
#    - mandatory preload fraction = 0.5 of selected chunk radius
#    - loading/leaving minimum time = 2 seconds
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
Add-Log $Log "    - preload_required_fraction = 0.5"
Add-Log $Log "    - world_loading_min_seconds = 2.0"
Add-Log $Log "    - world_leaving_min_seconds = 2.0"
Add-Log $Log "    - spawn_preload_radius/min_visible no longer define the main requirement"

# ----------------------------------------------------------------------
# 2. Renderer: remove black cube/overlay from panorama loading screen.
# ----------------------------------------------------------------------

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
        "Renderer::draw_menu_panorama_message without black cube" `
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
        throw "Cannot insert draw_menu_panorama_message: insertion marker not found"
    }

    $RendererCppText = $RendererCppText.Substring(0, $insertAt) + $RendererPanoramaMessageMethod + $RendererCppText.Substring($insertAt)
    $RendererCppChanged = $true
    Add-Log $Log "OK: inserted Renderer::draw_menu_panorama_message without black cube"
}

if ($RendererCppChanged) {
    Write-Utf8NoBom $RendererCppPath $RendererCppText
    Add-Log $Log "OK: src/render/renderer.cpp written"
}

# Ensure renderer.hpp has the transition method declaration.
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
        Add-Log $Log "OK: renderer.hpp added draw_menu_panorama_message declaration"
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
        Add-Log $Log "OK: renderer.hpp added unload_all_chunk_meshes declaration"
    } else {
        throw "Cannot patch renderer.hpp: unload_chunk_mesh declaration anchor not found"
    }
}

if ($RendererHppChanged) {
    Write-Utf8NoBom $RendererHppPath $RendererHppText
    Add-Log $Log "OK: src/render/renderer.hpp written"
}

# Ensure renderer.cpp has full unload method.
$RendererCppText = Read-Utf8Text $RendererCppPath
$RendererCppChanged2 = $false

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
        throw "Cannot insert unload_all_chunk_meshes: insertion marker not found"
    }

    $RendererCppText = $RendererCppText.Substring(0, $insertAt) + $RendererUnloadAllMethod + $RendererCppText.Substring($insertAt)
    $RendererCppChanged2 = $true
    Add-Log $Log "OK: renderer.cpp inserted unload_all_chunk_meshes"
}

if ($RendererCppChanged2) {
    Write-Utf8NoBom $RendererCppPath $RendererCppText
    Add-Log $Log "OK: src/render/renderer.cpp written after unload method insertion"
}

# ----------------------------------------------------------------------
# 3. Application: normal panorama speed, half-distance preload, 2 sec loading and leaving.
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

    for (int i = 0; i < frame_count && !platform_.should_close(); ++i) {
        platform_.pump_events();

        const float dt = platform_.frame_delta_seconds();
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
    "Application transition helpers with normal panorama speed" `
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

    const int full_preload_radius = selected_radius;

    const int spawn_block_x = static_cast<int>(std::floor(spawn_position.x));
    const int spawn_block_z = static_cast<int>(std::floor(spawn_position.z));
    const int probe_y = std::clamp(
        static_cast<int>(std::floor(spawn_position.y)) - 1,
        kWorldMinY,
        kWorldMaxY
    );

    const int warning_frame = world_runtime_tuning().spawn_preload_max_frames;
    const std::size_t requests_per_frame = world_runtime_tuning().spawn_preload_requests_per_frame;
    const std::size_t upload_max_count = world_runtime_tuning().spawn_preload_upload_max_count;
    const std::size_t required_area_chunks =
        static_cast<std::size_t>((required_preload_radius * 2 + 1) * (required_preload_radius * 2 + 1));

    std::vector<std::array<int, 2>> required_probe_offsets;
    required_probe_offsets.reserve(required_area_chunks);
    for (int dz = -required_preload_radius; dz <= required_preload_radius; ++dz) {
        for (int dx = -required_preload_radius; dx <= required_preload_radius; ++dx) {
            required_probe_offsets.push_back({{dx * kChunkWidth, dz * kChunkDepth}});
        }
    }

    bool spawn_column_loaded_once = false;
    bool required_area_ready_once = false;
    bool warning_logged = false;

    log_message(
        LogLevel::Info,
        std::string("Application: half-distance preload begin [selected_radius=") +
            std::to_string(selected_radius) +
            ", required_radius=" + std::to_string(required_preload_radius) +
            ", required_chunks=" + std::to_string(required_area_chunks) +
            ", min_seconds=" + std::to_string(world_runtime_tuning().world_loading_min_seconds) + "]"
    );

    for (int frame = 0; !platform_.should_close(); ++frame) {
        platform_.pump_events();

        const auto now = std::chrono::steady_clock::now();
        const float elapsed_seconds = std::chrono::duration<float>(now - loading_started).count();

        const int active_preload_radius = required_area_ready_once ? full_preload_radius : required_preload_radius;
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

        int loaded_probe_columns = 0;
        for (const auto& offset : required_probe_offsets) {
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

        required_area_ready_once =
            spawn_column_loaded_once &&
            loaded_probe_columns >= static_cast<int>(required_probe_offsets.size()) &&
            stats.visible_chunks >= required_area_chunks;

        if (required_area_ready_once &&
            elapsed_seconds >= world_runtime_tuning().world_loading_min_seconds) {
            log_message(
                LogLevel::Info,
                std::string("Application: half-distance preload done [frame=") +
                    std::to_string(frame) +
                    ", elapsed=" + std::to_string(elapsed_seconds) +
                    ", visible=" + std::to_string(stats.visible_chunks) +
                    ", probes=" + std::to_string(loaded_probe_columns) +
                    "/" + std::to_string(required_probe_offsets.size()) +
                    ", selected_radius=" + std::to_string(selected_radius) +
                    ", required_radius=" + std::to_string(required_preload_radius) + "]"
            );

            return true;
        }

        if (!warning_logged && frame >= warning_frame) {
            warning_logged = true;
            log_message(
                LogLevel::Warning,
                std::string("Application: half-distance preload is taking longer than expected [frame=") +
                    std::to_string(frame) +
                    ", visible=" + std::to_string(stats.visible_chunks) +
                    ", probes=" + std::to_string(loaded_probe_columns) +
                    "/" + std::to_string(required_probe_offsets.size()) + "]"
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
    "Application::preload_world_spawn half selected distance" `
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
    "Application::unload_world_for_menu two seconds and full release" `
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

if (-not $RuntimeTuningAfter.Contains("float preload_required_fraction {0.5f};")) {
    throw "Validation failed: preload_required_fraction missing"
}

if (-not $RuntimeTuningAfter.Contains("float world_loading_min_seconds {2.0f};")) {
    throw "Validation failed: world_loading_min_seconds missing"
}

if (-not $RuntimeTuningAfter.Contains("float world_leaving_min_seconds {2.0f};")) {
    throw "Validation failed: world_leaving_min_seconds missing"
}

if ($RendererCppAfter.Contains("height * 0.42f") -and $RendererCppAfter.Contains("draw_menu_panorama_message")) {
    throw "Validation failed: black overlay/cube may still exist in panorama message renderer"
}

if (-not $RendererCppAfter.Contains("text_x + 2.0f")) {
    throw "Validation failed: text shadow path missing"
}

if (-not $ApplicationCppAfter.Contains("selected_radius = std::max(1, world_streamer_->chunk_radius())")) {
    throw "Validation failed: selected render distance radius is not used"
}

if (-not $ApplicationCppAfter.Contains("preload_required_fraction")) {
    throw "Validation failed: preload_required_fraction not used in Application"
}

if (-not $ApplicationCppAfter.Contains("world_loading_min_seconds")) {
    throw "Validation failed: 2 second loading gate missing"
}

if (-not $ApplicationCppAfter.Contains("world_leaving_min_seconds")) {
    throw "Validation failed: 2 second leaving gate missing"
}

if (-not $ApplicationCppAfter.Contains("active_preload_radius = required_area_ready_once ? full_preload_radius : required_preload_radius")) {
    throw "Validation failed: fast-device extra preload behavior missing"
}

if (-not $ApplicationCppAfter.Contains("renderer_.resident_chunk_mesh_count() == 0")) {
    throw "Validation failed: resident chunk mesh cleanup check missing"
}

if (-not $ApplicationCppAfter.Contains("renderer_.unload_all_chunk_meshes();")) {
    throw "Validation failed: unload_all_chunk_meshes call missing"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/game/world_runtime_tuning.hpp"
Add-Log $Log "- src/render/renderer.hpp"
Add-Log $Log "- src/render/renderer.cpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Loading no longer waits for fixed 5x5."
Add-Log $Log "- Required preload radius is now ceil(selected_chunk_radius * 0.5)."
Add-Log $Log "- The loading screen stays for at least 2 seconds."
Add-Log $Log "- If required chunks are not ready after 2 seconds, loading continues."
Add-Log $Log "- If required chunks are ready early, the game keeps preloading farther chunks until 2 seconds pass."
Add-Log $Log "- Exit screen stays for at least 2 seconds and waits for renderer/world release."
Add-Log $Log "- Removed the black rectangle/cube behind transition text."
Add-Log $Log "- Panorama time now uses platform frame delta instead of fixed 1/60 increments."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Start world: the loading screen must stay at least 2 seconds."
Add-Log $Log "3. With chunk radius 16, required preload radius must be 8."
Add-Log $Log "4. Exit world: the exit screen must stay at least 2 seconds and return only after cleanup."
Add-Log $Log "5. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
