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

$Log = New-Object System.Collections.Generic.List[string]
$Log.Add("Patch report") | Out-Null
$Log.Add("Project root: " + $ProjectRoot) | Out-Null
$Log.Add("Backup dir: " + $BackupDir) | Out-Null
$Log.Add("Time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")) | Out-Null
$Log.Add("") | Out-Null

$WorldStreamerPath = Join-Path $ProjectRoot "src\game\world_streamer.cpp"

if (-not (Test-Path $WorldStreamerPath)) {
    throw "Required file not found: $WorldStreamerPath"
}

Backup-File $WorldStreamerPath

$text = Read-Utf8Text $WorldStreamerPath
$original = $text

# 1. Fix glued function boundary if it still exists.
$text = $text.Replace("}float completed_result_apply_budget_ms() {", "}`r`n`r`nfloat completed_result_apply_budget_ms() {")
$text = $text.Replace("}`nfloat completed_result_apply_budget_ms() {", "}`r`n`r`nfloat completed_result_apply_budget_ms() {")

# 2. Remove every duplicated kMaxDirtyChunkSavesPerTick declaration.
$constPattern = "\s*constexpr\s+std::size_t\s+kMaxDirtyChunkSavesPerTick\s*=\s*1\s*;"
$matchesBefore = [regex]::Matches($text, $constPattern).Count
$text = [regex]::Replace($text, $constPattern, "")

# 3. Insert exactly one declaration before grass update constants.
$grassPattern = "(?s)(#ifdef\s+__ANDROID__\s*constexpr\s+std::uint64_t\s+kGrassUpdateIntervalFrames)"
if (-not [regex]::IsMatch($text, $grassPattern)) {
    throw "Cannot find grass update constants marker for kMaxDirtyChunkSavesPerTick insertion"
}

$text = [regex]::Replace(
    $text,
    $grassPattern,
    "constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;`r`n`r`n`$1",
    1
)

# 4. Normalize function boundaries that may have been glued by previous marker replacement.
$text = $text.Replace("}`r`nint WorldStreamer::continuous_uploaded_radius", "}`r`n`r`nint WorldStreamer::continuous_uploaded_radius")
$text = $text.Replace("}`nint WorldStreamer::continuous_uploaded_radius", "}`r`n`r`nint WorldStreamer::continuous_uploaded_radius")
$text = $text.Replace("}`r`nvoid WorldStreamer::request_spawn_preload", "}`r`n`r`nvoid WorldStreamer::request_spawn_preload")
$text = $text.Replace("}`nvoid WorldStreamer::request_spawn_preload", "}`r`n`r`nvoid WorldStreamer::request_spawn_preload")
$text = $text.Replace("}`r`nvoid WorldStreamer::tick_generation_jobs", "}`r`n`r`nvoid WorldStreamer::tick_generation_jobs")
$text = $text.Replace("}`nvoid WorldStreamer::tick_generation_jobs", "}`r`n`r`nvoid WorldStreamer::tick_generation_jobs")

# 5. Validation.
$matchesAfter = [regex]::Matches($text, "constexpr\s+std::size_t\s+kMaxDirtyChunkSavesPerTick\s*=\s*1\s*;").Count
if ($matchesAfter -ne 1) {
    throw "Validation failed: kMaxDirtyChunkSavesPerTick declaration count is $matchesAfter, expected 1"
}

if ($text.Contains("}float completed_result_apply_budget_ms()")) {
    throw "Validation failed: glued completed_result_apply_budget_ms boundary still exists"
}

if ($text -eq $original) {
    $Log.Add("No text changes were needed.") | Out-Null
} else {
    Write-Utf8NoBom $WorldStreamerPath $text
    $Log.Add("Changed files:") | Out-Null
    $Log.Add("- src/game/world_streamer.cpp") | Out-Null
    $Log.Add("") | Out-Null
    $Log.Add("What changed:") | Out-Null
    $Log.Add("- Removed duplicated kMaxDirtyChunkSavesPerTick declarations.") | Out-Null
    $Log.Add("- Inserted exactly one kMaxDirtyChunkSavesPerTick declaration before grass update constants.") | Out-Null
    $Log.Add("- Fixed glued helper/function boundaries left by previous marker replacement.") | Out-Null
    $Log.Add("- Declarations before fix: " + $matchesBefore) | Out-Null
    $Log.Add("- Declarations after fix: " + $matchesAfter) | Out-Null
}

$Log.Add("") | Out-Null
$Log.Add("Next step:") | Out-Null
$Log.Add("1. Run build.bat or build_release.bat again.") | Out-Null
$Log.Add("2. If compilation fails again, send the first compiler error and 30 lines after it.") | Out-Null

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
