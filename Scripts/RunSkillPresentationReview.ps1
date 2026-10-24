<#
正式 CombatTest、独立服务器与双客户端的技能表现取证。
不替换 HUD/GameMode，不设置视觉通过；引擎按实际请求时刻捕获画面，原始帧全部保留。
#>
[CmdletBinding()]
param(
    [string]$EditorPath = 'E:/program/ue554/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe',
    [string]$OutputDirectory = '',
    [string]$FFmpegPath = 'C:/UEBuild/VideoInspectionTools/imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe',
    [ValidateRange(0,65535)][int]$Port = 0,
    [ValidateSet('Normal','Moderate','Harsh')][string]$Profile = 'Normal',
    [switch]$PlanOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('Saved/SkillPresentationReview/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $FFmpegPath -PathType Leaf)) { throw "Existing FFmpeg not found: $FFmpegPath" }
$launch = @{ EditorPath=$EditorPath; Map='/Game/Combat/Maps/CombatTest'; Port=$Port; Profile=$Profile;
    Play=$true; SkillDemo=$true; SkillReview=$true; ReviewOutput=$OutputDirectory; TimeoutSeconds=150; PlanOnly=[bool]$PlanOnly }
if ($PlanOnly) {
    & (Join-Path $PSScriptRoot 'RunLocalDedicatedServer.ps1') @launch
    Write-Output "Review output: $OutputDirectory | existing encoder: $FFmpegPath | capture: 960x540; actual-time VFR; visual pass requires inspection."
    return
}
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory -Force | Select-Object -First 1)) {
    throw 'Use a new or empty review output directory; previous evidence is never overwritten.'
}
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$utf8 = New-Object Text.UTF8Encoding($false)
$culture = [Globalization.CultureInfo]::InvariantCulture
$started = [DateTime]::UtcNow.ToString('o')
$module = Join-Path $projectRoot 'Binaries/Win64/UnrealEditor-multiplayer.dll'
$moduleBefore = if (Test-Path -LiteralPath $module) { (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
$failure = $null
$roleReports = @()
try {
    & (Join-Path $PSScriptRoot 'RunLocalDedicatedServer.ps1') @launch
    foreach ($role in @('Owner','Observer')) {
        $completePath = Join-Path $OutputDirectory "$role/complete.json"
        if (-not (Test-Path -LiteralPath $completePath -PathType Leaf)) { throw "Missing final $role engine manifest." }
        $record = Get-Content -LiteralPath $completePath -Raw | ConvertFrom-Json
        if ($record.role -cne $role -or -not $record.complete) { throw "$role completion identity is invalid." }
        $frames = @($record.frames | Where-Object { $_.written -and (Test-Path -LiteralPath $_.path -PathType Leaf) } | Sort-Object timeSeconds)
        if ($frames.Count -lt 2) { throw "$role has fewer than two written frames." }
        $roleRoot = [IO.Path]::GetFullPath((Join-Path $OutputDirectory $role)) + [IO.Path]::DirectorySeparatorChar
        $lines = [Collections.Generic.List[string]]::new()
        $lines.Add('ffconcat version 1.0')
        for ($index=0; $index -lt $frames.Count; ++$index) {
            $framePath = [IO.Path]::GetFullPath([string]$frames[$index].path)
            if (-not $framePath.StartsWith($roleRoot, [StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetExtension($framePath) -ine '.png') {
                throw 'Frame is outside its owned evidence directory or is not PNG.'
            }
            $nextTime = if ($index + 1 -lt $frames.Count) { [double]$frames[$index+1].timeSeconds } else { [double]$record.recording_duration_seconds }
            $duration = $nextTime - [double]$frames[$index].timeSeconds
            if ($duration -le 0) { throw "$role frame timestamps are not strictly increasing or exceed recording duration." }
            $escaped = $framePath.Replace('\','/').Replace("'", "'\''")
            $lines.Add("file '$escaped'")
            # PNG demuxer使用毫秒时间基；时长来自实际请求时间，不把卡顿画面加速伪装成固定帧率。
            $lines.Add('option framerate 1000')
            $lines.Add('duration ' + $duration.ToString('0.000000', $culture))
        }
        $lastPath = ([IO.Path]::GetFullPath([string]$frames[-1].path)).Replace('\','/').Replace("'", "'\''")
        $lines.Add("file '$lastPath'"); $lines.Add('option framerate 1000')
        $concatPath = Join-Path $OutputDirectory "$role/frames.ffconcat"
        [IO.File]::WriteAllLines($concatPath, $lines, $utf8)
        $video = Join-Path $OutputDirectory "$role/Review.mp4"
        $encoderLog = Join-Path $OutputDirectory "$role/Encode.log"
        $encoderArgs = @('-hide_banner','-nostdin','-n','-f','concat','-safe','0','-i',$concatPath,
            '-an','-vf','scale=960:540:force_original_aspect_ratio=decrease,pad=960:540:(ow-iw)/2:(oh-ih)/2',
            '-c:v','libx264','-preset','veryfast','-crf','20','-pix_fmt','yuv420p','-fps_mode','vfr','-movflags','+faststart',$video)
        $encoderLine = ($encoderArgs | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' '
        $encoder = Start-Process -FilePath $FFmpegPath -ArgumentList $encoderLine -WindowStyle Hidden -PassThru -Wait `
            -RedirectStandardError $encoderLog -RedirectStandardOutput (Join-Path $OutputDirectory "$role/Encode.stdout.log")
        if ($encoder.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $video)) { throw "$role video encoding failed; PNG and timing evidence retained." }
        $roleReports += [ordered]@{ role=$role; complete=[bool]$record.complete; execution_passed=[bool]$record.execution_passed;
            visual_passed=$false; manifest=$completePath; video=$video; written_frames=$frames.Count;
            requested_frames=@($record.frames).Count; first_frame_seconds=$frames[0].timeSeconds;
            recording_duration_seconds=$record.recording_duration_seconds; unsupported=$record.unsupported; failure=$record.failure;
            timing='Variable frame rate from actual engine request timestamps; PNG write time is recorded separately.' }
    }
} catch { $failure = $_.Exception.Message }
$sessionPath = Join-Path $OutputDirectory 'Session.json'
$session = if (Test-Path -LiteralPath $sessionPath) { Get-Content -LiteralPath $sessionPath -Raw | ConvertFrom-Json } else { $null }
$moduleAfter = if (Test-Path -LiteralPath $module) { (Get-FileHash -LiteralPath $module -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
$report = [ordered]@{ started_utc=$started; ended_utc=[DateTime]::UtcNow.ToString('o'); project=$projectRoot;
    map='/Game/Combat/Maps/CombatTest'; capture_width=960; capture_height=540; profile=$Profile; session=$session;
    build_module=$module; build_module_sha256_before=$moduleBefore; build_module_sha256_after=$moduleAfter;
    loaded_module_hash_verified=$false; module_unchanged_during_review=($moduleBefore -eq $moduleAfter);
    roles=$roleReports; execution_passed=($null -eq $failure -and $roleReports.Count -eq 2 -and @($roleReports | Where-Object { -not $_.execution_passed }).Count -eq 0);
    visual_passed=$false; visual_status='Awaiting inspection of both actual game recordings; no fixture HUD/GameMode replacement.';
    failure=$failure; evidence_retained=$true }
$reportPath = Join-Path $OutputDirectory 'ReviewReport.json'
[IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 30), $utf8)
Write-Output "Skill presentation review evidence: $reportPath"
if ($failure) { throw $failure }
