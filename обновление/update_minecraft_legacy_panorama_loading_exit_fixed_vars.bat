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

    if ($Path.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0) {
        throw "Write-Utf8NoBom received an invalid path: $Path"
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

$RendererHppPath = Join-Path $ProjectRoot "src\render\renderer.hpp"
$RendererCppPath = Join-Path $ProjectRoot "src\render\renderer.cpp"
$ApplicationHppPath = Join-Path $ProjectRoot "src\app\application.hpp"
$ApplicationCppPath = Join-Path $ProjectRoot "src\app\application.cpp"

$RequiredFiles = @($RendererHppPath, $RendererCppPath, $ApplicationHppPath, $ApplicationCppPath)
foreach ($file in $RequiredFiles) {
    if (-not (Test-Path $file)) {
        throw "Required file not found: $file"
    }
}

Backup-File $RendererHppPath
Backup-File $RendererCppPath
Backup-File $ApplicationHppPath
Backup-File $ApplicationCppPath

# ----------------------------------------------------------------------
# 1. renderer.hpp: public panorama transition method.
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
        Add-Log $Log "OK: renderer.hpp added draw_menu_panorama_message declaration"
    } else {
        throw "Cannot patch renderer.hpp: draw_main_menu declaration anchor not found"
    }
} else {
    Add-Log $Log "OK: renderer.hpp draw_menu_panorama_message declaration already exists"
}

if ($RendererHppChanged) {
    Write-Utf8NoBom $RendererHppPath $RendererHppText
    Add-Log $Log "OK: src/render/renderer.hpp written"
}

# ----------------------------------------------------------------------
# 2. renderer.cpp: lobby panorama + centered transition text.
# ----------------------------------------------------------------------

$RendererCppText = Read-Utf8Text $RendererCppPath
$RendererCppChanged = $false

$RendererMethod = @'
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

    std::vector<Vertex> overlay_vertices;
    append_hud_rect_fill(
        overlay_vertices,
        0.0f,
        height * 0.42f,
        width,
        height * 0.58f,
        width,
        height,
        {0.0f, 0.0f, 0.0f}
    );

    upload_dynamic_buffer(menu_overlay_vertex_buffer_, overlay_vertices);
    menu_overlay_vertex_count_ = static_cast<std::uint32_t>(overlay_vertices.size());
    if (menu_overlay_vertex_count_ > 0) {
        draw_colored_buffer(frame, menu_overlay_vertex_buffer_, menu_overlay_vertex_count_, hotbar_fill_pipeline_);
    }

    if (menu_font_.loaded && !message.empty()) {
        constexpr float text_height = 30.0f;
        const float text_width = menu_font_text_width(message, text_height);
        const float text_x = (width - text_width) * 0.5f;
        const float text_y = (height - text_height) * 0.5f;

        std::vector<Vertex> text_vertices;
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

if (-not $RendererCppText.Contains("void Renderer::draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message)")) {
    $insertMarker = "void Renderer::draw_pause_menu"
    $insertAt = $RendererCppText.IndexOf($insertMarker)

    if ($insertAt -lt 0) {
        $insertMarker = "bool Renderer::upload_chunk_mesh"
        $insertAt = $RendererCppText.IndexOf($insertMarker)
    }

    if ($insertAt -lt 0) {
        throw "Cannot insert draw_menu_panorama_message: renderer insertion marker not found"
    }

    $RendererCppText = $RendererCppText.Substring(0, $insertAt) + $RendererMethod + $RendererCppText.Substring($insertAt)
    $RendererCppChanged = $true
    Add-Log $Log "OK: renderer.cpp inserted draw_menu_panorama_message implementation"
} else {
    Add-Log $Log "OK: renderer.cpp draw_menu_panorama_message implementation already exists"
}

if ($RendererCppChanged) {
    Write-Utf8NoBom $RendererCppPath $RendererCppText
    Add-Log $Log "OK: src/render/renderer.cpp written"
}

# ----------------------------------------------------------------------
# 3. application.hpp: transition helper declaration.
# ----------------------------------------------------------------------

$ApplicationHppText = Read-Utf8Text $ApplicationHppPath
$ApplicationHppChanged = $false

if (-not $ApplicationHppText.Contains("void render_world_transition_frames(int frame_count, const char* message);")) {
    $oldDecl = '    void render_black_transition_frames(int frame_count);'
    $newDecl = @'
    void render_black_transition_frames(int frame_count);
    void render_world_transition_frames(int frame_count, const char* message);
'@

    if ($ApplicationHppText.Contains($oldDecl)) {
        $ApplicationHppText = $ApplicationHppText.Replace($oldDecl, $newDecl.TrimEnd())
        $ApplicationHppChanged = $true
        Add-Log $Log "OK: application.hpp added render_world_transition_frames declaration"
    } else {
        throw "Cannot patch application.hpp: render_black_transition_frames declaration anchor not found"
    }
} else {
    Add-Log $Log "OK: application.hpp render_world_transition_frames declaration already exists"
}

if ($ApplicationHppChanged) {
    Write-Utf8NoBom $ApplicationHppPath $ApplicationHppText
    Add-Log $Log "OK: src/app/application.hpp written"
}

# ----------------------------------------------------------------------
# 4. application.cpp: use panorama transition screens.
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
        menu_time_seconds_ += 1.0f / 60.0f;

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
    "Application transition helpers" `
    ([ref]$ApplicationCppChanged) `
    $Log

$UnloadFunction = @'
void Application::unload_world_for_menu() {
    render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Выход из мира");

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

    render_world_transition_frames(world_runtime_tuning().transition_black_frames, "Выход из мира");
}

'@

$ApplicationCppText = Replace-BetweenMarkers `
    $ApplicationCppText `
    "void Application::unload_world_for_menu() {" `
    "Renderer::CaveVisibilityFrame Application::update_cave_visibility_frame" `
    $UnloadFunction `
    "Application::unload_world_for_menu" `
    ([ref]$ApplicationCppChanged) `
    $Log

if ($ApplicationCppChanged) {
    Write-Utf8NoBom $ApplicationCppPath $ApplicationCppText
    Add-Log $Log "OK: src/app/application.cpp written"
}

# ----------------------------------------------------------------------
# 5. Validation.
# ----------------------------------------------------------------------

$RendererHppAfter = Read-Utf8Text $RendererHppPath
$RendererCppAfter = Read-Utf8Text $RendererCppPath
$ApplicationHppAfter = Read-Utf8Text $ApplicationHppPath
$ApplicationCppAfter = Read-Utf8Text $ApplicationCppPath

if (-not $RendererHppAfter.Contains("void draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message);")) {
    throw "Validation failed: renderer.hpp draw_menu_panorama_message declaration missing"
}

if (-not $RendererCppAfter.Contains("void Renderer::draw_menu_panorama_message(float time_seconds, bool use_night_panorama, const std::string& message)")) {
    throw "Validation failed: renderer.cpp draw_menu_panorama_message implementation missing"
}

if (-not $RendererCppAfter.Contains("append_menu_font_text(")) {
    throw "Validation failed: centered text rendering path missing"
}

if (-not $ApplicationHppAfter.Contains("void render_world_transition_frames(int frame_count, const char* message);")) {
    throw "Validation failed: application.hpp render_world_transition_frames declaration missing"
}

if (-not $ApplicationCppAfter.Contains('"Загрузка мира"')) {
    throw "Validation failed: loading text missing"
}

if (-not $ApplicationCppAfter.Contains('"Выход из мира"')) {
    throw "Validation failed: exit text missing"
}

if (-not $ApplicationCppAfter.Contains("renderer_.draw_menu_panorama_message(")) {
    throw "Validation failed: application.cpp does not call draw_menu_panorama_message"
}

if (-not $ApplicationCppAfter.Contains("world_streamer_->flush_all_dirty_chunks();")) {
    throw "Validation failed: world save flush missing during exit"
}

if (-not $ApplicationCppAfter.Contains("world_streamer_.reset();")) {
    throw "Validation failed: world_streamer reset missing during exit"
}

if (-not $ApplicationCppAfter.Contains("world_save_.reset();")) {
    throw "Validation failed: world_save reset missing during exit"
}

Add-Log $Log ""
Add-Log $Log "Changed files:"
Add-Log $Log "- src/render/renderer.hpp"
Add-Log $Log "- src/render/renderer.cpp"
Add-Log $Log "- src/app/application.hpp"
Add-Log $Log "- src/app/application.cpp"
Add-Log $Log ""
Add-Log $Log "What changed:"
Add-Log $Log "- Loading and exit screens now use the lobby panorama."
Add-Log $Log "- Loading text: Загрузка мира."
Add-Log $Log "- Exit text: Выход из мира."
Add-Log $Log "- Existing world preload/save/unload logic remains in Application."
Add-Log $Log "- Fixed PowerShell variable naming: all paths use *Path variables, all file text uses *Text variables."
Add-Log $Log ""
Add-Log $Log "Next step:"
Add-Log $Log "1. Run build_release.bat."
Add-Log $Log "2. Enter the world and check the panorama loading screen."
Add-Log $Log "3. Exit the world and check the panorama exit screen."
Add-Log $Log "4. If build fails, send the first compiler error and 30 lines after it."

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
