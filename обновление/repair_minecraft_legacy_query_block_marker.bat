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

$Log = New-Object System.Collections.Generic.List[string]
$Log.Add("Patch report") | Out-Null
$Log.Add("Project root: " + $ProjectRoot) | Out-Null
$Log.Add("Backup dir: " + $BackupDir) | Out-Null
$Log.Add("Time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")) | Out-Null
$Log.Add("") | Out-Null

$WorldStreamerCpp = Join-Path $ProjectRoot "src\game\world_streamer.cpp"

if (-not (Test-Path $WorldStreamerCpp)) {
    throw "Required file not found: $WorldStreamerCpp"
}

Backup-File $WorldStreamerCpp

$text = Read-Utf8Text $WorldStreamerCpp
$original = $text

# Fix accidental duplicated marker insertion from the previous stats patch:
# BlockQueryResult WorldStreamer::query_block_at_worldBlockQueryResult WorldStreamer::query_block_at_world(...)
$badExact = "BlockQueryResult WorldStreamer::query_block_at_worldBlockQueryResult WorldStreamer::query_block_at_world"
$goodExact = "BlockQueryResult WorldStreamer::query_block_at_world"

$exactCount = ([regex]::Matches([regex]::Escape($text), [regex]::Escape($badExact))).Count
$text = $text.Replace($badExact, $goodExact)

# More tolerant fallback in case spaces/newlines changed.
$badPattern = "BlockQueryResult\s+WorldStreamer::query_block_at_world\s*BlockQueryResult\s+WorldStreamer::query_block_at_world"
$regexCount = [regex]::Matches($text, $badPattern).Count
$text = [regex]::Replace($text, $badPattern, $goodExact)

# Normalize local function boundaries around stats and query block.
$text = $text.Replace("}`r`nBlockQueryResult WorldStreamer::query_block_at_world", "}`r`n`r`nBlockQueryResult WorldStreamer::query_block_at_world")
$text = $text.Replace("}`nBlockQueryResult WorldStreamer::query_block_at_world", "}`r`n`r`nBlockQueryResult WorldStreamer::query_block_at_world")

# Validation.
if ($text.Contains("query_block_at_worldBlockQueryResult")) {
    throw "Validation failed: query_block_at_worldBlockQueryResult still exists"
}

$validCount = [regex]::Matches($text, "BlockQueryResult\s+WorldStreamer::query_block_at_world\s*\(").Count
if ($validCount -ne 1) {
    throw "Validation failed: query_block_at_world implementation count is $validCount, expected 1"
}

$statsCount = [regex]::Matches($text, "WorldStreamer::StreamingStats\s+WorldStreamer::stats\s*\(\)\s+const\s*\{").Count
if ($statsCount -ne 1) {
    throw "Validation failed: WorldStreamer::stats implementation count is $statsCount, expected 1"
}

if ($text -ne $original) {
    Write-Utf8NoBom $WorldStreamerCpp $text
    $Log.Add("Changed files:") | Out-Null
    $Log.Add("- src/game/world_streamer.cpp") | Out-Null
    $Log.Add("") | Out-Null
    $Log.Add("What changed:") | Out-Null
    $Log.Add("- Fixed duplicated query_block_at_world marker after WorldStreamer::stats patch.") | Out-Null
    $Log.Add("- Removed accidental query_block_at_worldBlockQueryResult symbol.") | Out-Null
    $Log.Add("- Normalized function boundary before query_block_at_world.") | Out-Null
    $Log.Add("- Exact duplicate replacements: " + $exactCount) | Out-Null
    $Log.Add("- Regex fallback replacements after exact pass: " + $regexCount) | Out-Null
} else {
    $Log.Add("No changes were needed; file already passes this repair validation.") | Out-Null
}

$Log.Add("") | Out-Null
$Log.Add("Next step:") | Out-Null
$Log.Add("1. Run build.bat again.") | Out-Null
$Log.Add("2. If another compiler error appears, send the first error and 30 lines after it.") | Out-Null

Write-Utf8NoBom $ReportPath (($Log -join [Environment]::NewLine) + [Environment]::NewLine)

Write-Host ""
Write-Host "Patch applied."
Write-Host "Project root: $ProjectRoot"
Write-Host "Backup dir: $BackupDir"
Write-Host "Report: $ReportPath"
Write-Host ""
Get-Content $ReportPath
