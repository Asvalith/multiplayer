[CmdletBinding()]
param(
    [ValidateSet("Normal", "Moderate", "Harsh", "All")]
    [string]$Profile = "Normal",

    [switch]$TestReconnect,

    [switch]$TestSession,

    [switch]$TestGameplayFlow,

    [switch]$TestRestartAndLeave,

    [ValidateSet("None", "Baseline", "Optimized")]
    [string]$BandwidthProfile = "None",

    [switch]$CompareBandwidth,

    [switch]$PublishEvidence,

    [string]$EvidenceName = "network-regression",

    [string]$EditorPath = "",

    [ValidateRange(1024, 65532)]
    [int]$Port = 27777,

    [ValidateRange(10, 180)]
    [int]$TimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectPath = Join-Path $ProjectRoot "multiplayer.uproject"
$TestMap = "/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo"
$LogRoot = Join-Path $ProjectRoot "Saved\Logs"
$ReportRoot = Join-Path $ProjectRoot "Saved\TestReports"

$Profiles = [ordered]@{
    Normal = [pscustomobject]@{ Name = "Normal"; LagMs = 0; VarianceMs = 0; LossPercent = 0 }
    Moderate = [pscustomobject]@{ Name = "Moderate"; LagMs = 100; VarianceMs = 20; LossPercent = 2 }
    Harsh = [pscustomobject]@{ Name = "Harsh"; LagMs = 200; VarianceMs = 50; LossPercent = 5 }
}

function Quote-Argument {
    param([string]$Value)
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Read-Log {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ""
    }

    $Stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        $Reader = New-Object System.IO.StreamReader($Stream)
        try {
            return $Reader.ReadToEnd()
        }
        finally {
            $Reader.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
}

function Get-FileSha256 {
    param([string]$Path)

    $Sha256 = [Security.Cryptography.SHA256]::Create()
    $Stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($Sha256.ComputeHash($Stream))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $Stream.Dispose()
        $Sha256.Dispose()
    }
}

function Get-SourceFingerprint {
    $Files = @()
    foreach ($Directory in @("Source", "Scripts", "Config")) {
        $Path = Join-Path $ProjectRoot $Directory
        if (Test-Path -LiteralPath $Path -PathType Container) {
            $Files += Get-ChildItem -LiteralPath $Path -File -Recurse
        }
    }
    $Files += Get-ChildItem -LiteralPath $ProjectRoot -File |
        Where-Object { $_.Name -eq "multiplayer.uproject" -or $_.Extension -eq ".bat" }

    $ManifestLines = @($Files | Sort-Object FullName | ForEach-Object {
        $RelativePath = $_.FullName.Substring($ProjectRoot.Length).TrimStart("\", "/")
        $FileHash = Get-FileSha256 -Path $_.FullName
        "$RelativePath`t$FileHash"
    })
    $ManifestBytes = [Text.Encoding]::UTF8.GetBytes(($ManifestLines -join "`n"))
    $Sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($Sha256.ComputeHash($ManifestBytes))).Replace("-", "").ToLowerInvariant()
    }
    finally {
        $Sha256.Dispose()
    }
}

function Wait-ForLog {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$ExpectedCount,
        [System.Diagnostics.Process]$Process,
        [string]$Description
    )

    $Deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $Deadline) {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "$Description failed because process $($Process.Id) exited."
        }

        $Text = Read-Log -Path $Path
        $RuntimeFailure = [regex]::Match(
            $Text,
            "(?im)(Coop automation failed:|Fatal error:|Ensure condition failed|LogMultiplayer:\s+Error:)")
        if ($RuntimeFailure.Success) {
            throw "$Description failed early because '$($RuntimeFailure.Value)' appeared in $Path."
        }
        if ([regex]::Matches($Text, $Pattern).Count -ge $ExpectedCount) {
            return
        }

        Start-Sleep -Milliseconds 250
    }

    $Tail = ((Read-Log -Path $Path) -split "\r?\n" | Select-Object -Last 20) -join [Environment]::NewLine
    throw "$Description timed out after $TimeoutSeconds seconds." + [Environment]::NewLine + $Tail
}

function Stop-Game {
    param([System.Diagnostics.Process]$Process)

    if ($null -eq $Process) {
        return
    }

    [int]$ProcessId = $Process.Id
    if ($null -eq (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
        return
    }

    # Windows PowerShell 5 的 Process.Kill() 对 UnrealEditor 偶尔不生效；taskkill 直接按 PID
    # 终止目标实例。这里不使用 /T，避免误伤该 UE 实例启动的共享 DDC/Trace 服务。
    $TaskKillPath = Join-Path $env:SystemRoot "System32\taskkill.exe"
    & $TaskKillPath /PID $ProcessId /F 2>$null | Out-Null
    [int]$TaskKillExitCode = $LASTEXITCODE

    [int]$StopAttempt = 0
    while ($StopAttempt -lt 100) {
        if ($null -eq (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
            return
        }

        Start-Sleep -Milliseconds 100
        $StopAttempt += 1
    }

    throw "Test process $ProcessId did not exit within 10 seconds after taskkill (exit code $TaskKillExitCode)."
}

function Get-NetworkArguments {
    param([pscustomobject]$NetworkProfile)

    $Arguments = @()
    if ($NetworkProfile.LagMs -gt 0) {
        $Arguments += "-PktLag=$($NetworkProfile.LagMs)"
        $Arguments += "-PktLagVariance=$($NetworkProfile.VarianceMs)"
        $Arguments += "-PktLoss=$($NetworkProfile.LossPercent)"
    }
    return $Arguments
}

function Start-Game {
    param(
        [string]$Address,
        [string]$LogPath,
        [pscustomobject]$NetworkProfile,
        [int]$TestPort,
        [string[]]$ExtraArguments = @(),
        [switch]$IsHost
    )

    $Arguments = @(
        (Quote-Argument -Value $ProjectPath),
        (Quote-Argument -Value $Address),
        "-game",
        "-NullRHI",
        "-unattended",
        "-NoSound",
        "-NoSplash",
        "-DDC=InstalledNoZenLocalFallback"
    )
    $Arguments += @(Get-NetworkArguments -NetworkProfile $NetworkProfile)
    $Arguments += $ExtraArguments
    if ($IsHost) {
        $Arguments += "-port=$TestPort"
    }
    $Arguments += Quote-Argument -Value "-abslog=$LogPath"

    return Start-Process -FilePath $EditorPath -ArgumentList $Arguments -PassThru -WindowStyle Hidden
}

function Assert-Logs {
    param(
        [string[]]$Paths,
        [pscustomobject]$NetworkProfile,
        [string]$ReconnectClientLog = "",
        [bool]$ValidateGameplayFlow = $false
    )

    $CleanupMarker = "Local session cleaned; returning to the main menu."
    $MenuArrivalMarker = "Leave transition completed in the main menu:"
    [int]$CleanupIndex = -1
    [int]$MenuArrivalIndex = -1
    foreach ($Path in $Paths) {
        $Text = Read-Log -Path $Path
        if ($Path -eq $ReconnectClientLog) {
            $Text = [regex]::Replace(
                $Text,
                "(?im)^.*Development reconnect test.*(?:\r?\n)?",
                "")
        }
        $FailureScanText = $Text
        $CleanupIndex = $Text.IndexOf($CleanupMarker, [StringComparison]::Ordinal)
        $MenuArrivalIndex = $Text.IndexOf($MenuArrivalMarker, [StringComparison]::Ordinal)
        if ($CleanupIndex -ge 0 -and $MenuArrivalIndex -gt $CleanupIndex) {
            # 主动退出拆网会产生三种固定的 LogNet ConnectionLost 行；只删除这些预期行。
            # cleanup 之后的 Fatal、Ensure 或项目错误仍保留，不能截断整段日志造成假通过。
            $ExpectedLeaveDisconnectPattern =
                "(?im)^.*LogNet:\s+(?:Error:\s+UEngine::BroadcastNetworkFailure:\s+FailureType\s*=\s*ConnectionLost|Warning:\s+Network Failure:\s+GameNetDriver\[ConnectionLost\]|NetworkFailure:\s+ConnectionLost).*(?:\r?\n|$)"
            $FailureScanText =
                $Text.Substring(0, $CleanupIndex) +
                [regex]::Replace(
                    $Text.Substring($CleanupIndex),
                    $ExpectedLeaveDisconnectPattern,
                    "")
        }

        $FailurePattern =
            "(?im)(Fatal error:|Ensure condition failed|LogMultiplayer:\s+Error:|LogNet:\s+Error:|LogPlayerController:\s+Error:|NetworkFailure:|TravelFailure:)"
        $Failure = [regex]::Match($FailureScanText, $FailurePattern)
        if ($Failure.Success) {
            throw "Runtime failure marker '$($Failure.Value)' was found in $Path."
        }

        if ($ValidateGameplayFlow) {
            $VictoryTransitionCount = [regex]::Matches(
                $Text,
                "Coop objective: victory transition broadcast\.").Count
            if ($VictoryTransitionCount -ne 1) {
                throw "Expected exactly one victory transition in $Path, found $VictoryTransitionCount."
            }
        }

        if ($NetworkProfile.LagMs -gt 0) {
            if ($Text -notmatch "PktLag set to $($NetworkProfile.LagMs)") {
                throw "Packet lag was not applied in $Path."
            }
            if ($Text -notmatch "PktLoss set to $($NetworkProfile.LossPercent)") {
                throw "Packet loss was not applied in $Path."
            }
        }
    }
}

function Invoke-NetworkTest {
    param(
        [pscustomobject]$NetworkProfile,
        [int]$TestPort,
        [string]$TestBandwidthProfile
    )

    $StartedAt = Get-Date
    $RunId = "{0}-{1}" -f $StartedAt.ToString("yyyyMMdd-HHmmss-fff"), $NetworkProfile.Name
    $HostLog = Join-Path $LogRoot "$RunId-Host.log"
    $ClientLog = Join-Path $LogRoot "$RunId-Client.log"
    $Processes = @()
    $Evidence = @()
    $ErrorMessage = $null
    $Success = $false
    $BandwidthSample = $null

    try {
        Write-Host "[$($NetworkProfile.Name)] Starting listen server on port $TestPort..."
        $HostArguments = @()
        if ($TestSession) {
            $HostArguments += "-CoopTestHostSession"
            $HostArguments += "-CoopTestToken=$RunId"
        }
        if ($TestGameplayFlow) { $HostArguments += "-CoopTestGameplayFlow" }
        if ($TestRestartAndLeave) { $HostArguments += "-CoopTestRestart" }
        if ($TestBandwidthProfile -ne "None") {
            $HostArguments += "-CoopBandwidthProfile=$TestBandwidthProfile"
        }
        $HostProcess = Start-Game -Address ($TestMap + "?listen") -LogPath $HostLog -NetworkProfile $NetworkProfile -TestPort $TestPort -ExtraArguments $HostArguments -IsHost
        $Processes += $HostProcess
        Wait-ForLog -Path $HostLog -Pattern "Coop objective configured: RequiredKeys=4" -ExpectedCount 1 -Process $HostProcess -Description "Listen server startup"
        $Evidence += "Server initialized RequiredKeys=4"

        if ($TestSession) {
            Wait-ForLog -Path $HostLog -Pattern "Session automation: host session advertised\." -ExpectedCount 1 -Process $HostProcess -Description "Session advertisement"
            $Evidence += "OnlineSubsystemNull advertised the current listen map without automatic map travel"
        }

        Write-Host "[$($NetworkProfile.Name)] Starting client..."
        $ClientArguments = @()
        if ($TestReconnect) { $ClientArguments += "-CoopTestReconnect" }
        if ($TestSession) {
            $ClientArguments += "-CoopTestJoinSession"
            $ClientArguments += "-CoopTestToken=$RunId"
        }
        $ClientAddress = if ($TestSession) { "/Game/UI/mainmenu" } else { "127.0.0.1:$TestPort" }
        $ClientProcess = Start-Game -Address $ClientAddress -LogPath $ClientLog -NetworkProfile $NetworkProfile -TestPort $TestPort -ExtraArguments $ClientArguments
        $Processes += $ClientProcess
        if ($TestSession) {
            Wait-ForLog -Path $ClientLog -Pattern "Session automation: found [1-9][0-9]* advertised session\(s\)\." -ExpectedCount 1 -Process $ClientProcess -Description "Session discovery"
            Wait-ForLog -Path $ClientLog -Pattern "Session automation: client joined the advertised current map\." -ExpectedCount 1 -Process $ClientProcess -Description "Session join"
            $Evidence += "Client discovered the LAN session, resolved its address, and joined it"
        }
        else {
            Wait-ForLog -Path $ClientLog -Pattern "Welcomed by server \(Level: /Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo" -ExpectedCount 1 -Process $ClientProcess -Description "Client connection"
        }
        Wait-ForLog -Path $HostLog -Pattern "Join succeeded:" -ExpectedCount 1 -Process $HostProcess -Description "Server join acknowledgement"
        $Evidence += "Client joined the cooperative map"

        $Logs = @($HostLog, $ClientLog)
        if ($NetworkProfile.LagMs -gt 0) {
            $Evidence += "Packet simulation settings were applied on both peers"
        }

        if ($TestReconnect) {
            Write-Host "[$($NetworkProfile.Name)] Waiting for the same client process to reconnect..."
            Wait-ForLog -Path $ClientLog -Pattern "Automatic reconnect succeeded after" -ExpectedCount 1 -Process $ClientProcess -Description "Automatic reconnect"
            Wait-ForLog -Path $HostLog -Pattern "Join succeeded:" -ExpectedCount 2 -Process $HostProcess -Description "Server reconnect acknowledgement"
            $Evidence += "The same client process automatically reconnected after an injected ConnectionLost failure event"
        }

        if ($TestGameplayFlow) {
            Wait-ForLog -Path $HostLog -Pattern "Coop automation: key/socket authority and duplicate guard passed\." -ExpectedCount 1 -Process $HostProcess -Description "Key and socket flow"
            Wait-ForLog -Path $HostLog -Pattern "Coop automation: pressure plate and gate flow passed\." -ExpectedCount 1 -Process $HostProcess -Description "Pressure plate and gate flow"
            Wait-ForLog -Path $HostLog -Pattern "Coop automation: server-driven moving platform flow passed\." -ExpectedCount 1 -Process $HostProcess -Description "Moving platform flow"
            Wait-ForLog -Path $HostLog -Pattern "Coop automation: FULL GAMEPLAY FLOW PASS\." -ExpectedCount 1 -Process $HostProcess -Description "Authoritative victory flow"
            Wait-ForLog -Path $ClientLog -Pattern "Victory UI displayed with restart and leave actions\." -ExpectedCount 1 -Process $ClientProcess -Description "Client victory UI"
            $Evidence += "Server actors completed key/socket state and duplicate guards, plate, gate, platform endpoint, win-area and client victory UI paths"

            if ($TestBandwidthProfile -ne "None") {
                Wait-ForLog -Path $HostLog -Pattern "Coop bandwidth sample: Profile=$TestBandwidthProfile" -ExpectedCount 1 -Process $HostProcess -Description "Bandwidth sample"
                $HostText = Read-Log -Path $HostLog
                $Match = [regex]::Match(
                    $HostText,
                    "Coop bandwidth sample: Profile=$TestBandwidthProfile DurationSeconds=([0-9.]+) OutBytes=([0-9]+) BytesPerSecond=([0-9.]+)")
                if (-not $Match.Success) {
                    throw "Bandwidth sample could not be parsed from $HostLog."
                }
                $BandwidthSample = [ordered]@{
                    Profile = $TestBandwidthProfile
                    Scope = "Server total outgoing bytes during the deterministic two-player gameplay flow"
                    MovingPlatformMaxNetUpdateHz = if ($TestBandwidthProfile -eq "Baseline") { 100 } else { 30 }
                    DurationSeconds = [double]$Match.Groups[1].Value
                    OutBytes = [int64]$Match.Groups[2].Value
                    BytesPerSecond = [double]$Match.Groups[3].Value
                }
                $Evidence += "Captured real UNetDriver outgoing bytes for the $TestBandwidthProfile A/B profile"
            }
        }

        if ($TestRestartAndLeave) {
            Wait-ForLog -Path $HostLog -Pattern "Restarting the current coop map after an authoritative victory\." -ExpectedCount 1 -Process $HostProcess -Description "Current-map restart request"
            Wait-ForLog -Path $HostLog -Pattern "Coop automation: current-map restart completed\." -ExpectedCount 1 -Process $HostProcess -Description "Current-map restart completion"
            Wait-ForLog -Path $HostLog -Pattern "Coop automation: initiating complete session leave\." -ExpectedCount 1 -Process $HostProcess -Description "Host leave"
            Wait-ForLog -Path $HostLog -Pattern "Local session cleaned; returning to the main menu\." -ExpectedCount 1 -Process $HostProcess -Description "Host session cleanup"
            Wait-ForLog -Path $ClientLog -Pattern "Local session cleaned; returning to the main menu\." -ExpectedCount 1 -Process $ClientProcess -Description "Client session cleanup"
            Wait-ForLog -Path $HostLog -Pattern "Leave transition completed in the main menu: /Game/UI/mainmenu" -ExpectedCount 1 -Process $HostProcess -Description "Host menu arrival"
            Wait-ForLog -Path $ClientLog -Pattern "Leave transition completed in the main menu: /Game/UI/mainmenu" -ExpectedCount 1 -Process $ClientProcess -Description "Client menu arrival"
            $Evidence += "Server reloaded only the current map, then host and client cleaned their sessions and returned to the menu"
        }

        # 最终状态已经由上面的明确日志标记确认。先按客户端到主机的逆序停止进程，
        # 冻结日志后再做全量错误扫描，避免 UE 仍在追加拆网消息时产生不一致快照。
        Stop-Game -Process $ClientProcess
        Stop-Game -Process $HostProcess
        $Processes = @()

        $ExpectedDisconnectLog = if ($TestReconnect) { $ClientLog } else { "" }
        Assert-Logs -Paths $Logs -NetworkProfile $NetworkProfile -ReconnectClientLog $ExpectedDisconnectLog -ValidateGameplayFlow ([bool]$TestGameplayFlow)
        $Success = $true
    }
    catch {
        $ErrorMessage = $_.Exception.Message
    }
    finally {
        foreach ($Process in $Processes) {
            try {
                Stop-Game -Process $Process
            }
            catch {
                # 一个实例清理失败时仍继续终止其余 PID，并把清理错误写入本组测试结果。
                $Success = $false
                $CleanupError = $_.Exception.Message
                if ([string]::IsNullOrWhiteSpace($ErrorMessage)) {
                    $ErrorMessage = $CleanupError
                }
                else {
                    $ErrorMessage += " | Cleanup: $CleanupError"
                }
            }
        }
    }

    $FinishedAt = Get-Date
    return [pscustomobject][ordered]@{
        Profile = $NetworkProfile.Name
        Port = $TestPort
        LagMs = $NetworkProfile.LagMs
        LagVarianceMs = $NetworkProfile.VarianceMs
        LossPercent = $NetworkProfile.LossPercent
        ReconnectTested = [bool]$TestReconnect
        SessionTested = [bool]$TestSession
        GameplayFlowTested = [bool]$TestGameplayFlow
        RestartAndLeaveTested = [bool]$TestRestartAndLeave
        BandwidthProfile = $TestBandwidthProfile
        BandwidthSample = $BandwidthSample
        Success = $Success
        DurationSeconds = [math]::Round(($FinishedAt - $StartedAt).TotalSeconds, 2)
        Logs = @($HostLog, $ClientLog)
        Evidence = $Evidence
        Error = $ErrorMessage
    }
}

if ([string]::IsNullOrWhiteSpace($EditorPath)) {
    $EditorPath = $env:UE_EDITOR
}
if ([string]::IsNullOrWhiteSpace($EditorPath) -or
    -not (Test-Path -LiteralPath $EditorPath -PathType Leaf)) {
    throw "UnrealEditor.exe was not found. Pass -EditorPath or set UE_EDITOR."
}
if (-not (Test-Path -LiteralPath $ProjectPath -PathType Leaf)) {
    throw "Project file was not found: $ProjectPath"
}
if ($TestReconnect -and ($TestGameplayFlow -or $TestRestartAndLeave)) {
    throw "Reconnect and full gameplay/restart tests use different timelines; run them as separate invocations."
}
if ($TestRestartAndLeave -and -not $TestGameplayFlow) {
    throw "TestRestartAndLeave requires TestGameplayFlow so victory can be reached first."
}
if ($CompareBandwidth -and -not $TestGameplayFlow) {
    throw "CompareBandwidth requires TestGameplayFlow."
}
if ($CompareBandwidth -and $Profile -eq "All") {
    throw "CompareBandwidth accepts one network profile at a time; choose Normal, Moderate, or Harsh."
}
if ($CompareBandwidth -and $BandwidthProfile -ne "None") {
    throw "Do not combine CompareBandwidth with BandwidthProfile; comparison already runs Baseline and Optimized."
}

New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
New-Item -ItemType Directory -Force -Path $ReportRoot | Out-Null

if ($Profile -eq "All") {
    [object[]]$ProfilesToRun = @($Profiles.Values)
}
else {
    [object[]]$ProfilesToRun = @($Profiles[$Profile])
}

$BandwidthProfilesToRun = if ($CompareBandwidth) { @("Baseline", "Optimized") } else { @($BandwidthProfile) }
$Results = @()
$RunIndex = 0
foreach ($CurrentProfile in $ProfilesToRun) {
    foreach ($CurrentBandwidthProfile in $BandwidthProfilesToRun) {
        $Result = Invoke-NetworkTest -NetworkProfile $CurrentProfile -TestPort ($Port + $RunIndex) -TestBandwidthProfile $CurrentBandwidthProfile
        ++$RunIndex
        $Results += $Result
        $Label = if ($CurrentBandwidthProfile -eq "None") { $Result.Profile } else { "$($Result.Profile)/$CurrentBandwidthProfile" }
        if ($Result.Success) {
            Write-Host "[$Label] PASS in $($Result.DurationSeconds)s" -ForegroundColor Green
        }
        else {
            Write-Host "[$Label] FAIL: $($Result.Error)" -ForegroundColor Red
        }
    }
}

$Failures = @($Results | Where-Object { -not $_.Success })
$Summary = [ordered]@{
    Project = $ProjectPath
    Editor = $EditorPath
    RequestedProfile = $Profile
    ReconnectTested = [bool]$TestReconnect
    SessionTested = [bool]$TestSession
    GameplayFlowTested = [bool]$TestGameplayFlow
    RestartAndLeaveTested = [bool]$TestRestartAndLeave
    Success = $Failures.Count -eq 0
    Results = $Results
    BandwidthComparison = $null
}

if ($CompareBandwidth) {
    $BaselineResult = $Results | Where-Object { $_.BandwidthProfile -eq "Baseline" -and $_.Success } | Select-Object -First 1
    $OptimizedResult = $Results | Where-Object { $_.BandwidthProfile -eq "Optimized" -and $_.Success } | Select-Object -First 1
    if ($null -ne $BaselineResult -and $null -ne $OptimizedResult) {
        $BaselineBps = [double]$BaselineResult.BandwidthSample.BytesPerSecond
        $OptimizedBps = [double]$OptimizedResult.BandwidthSample.BytesPerSecond
        $Reduction = if ($BaselineBps -gt 0) {
            [math]::Round((1.0 - ($OptimizedBps / $BaselineBps)) * 100.0, 2)
        } else { 0.0 }
        $Summary.BandwidthComparison = [ordered]@{
            Method = "Controlled A/B on the same deterministic flow; Baseline raises moving-platform network update cap from the project setting of 30Hz to 100Hz. This is not a historical-version claim."
            BaselineBytesPerSecond = $BaselineBps
            OptimizedBytesPerSecond = $OptimizedBps
            ReductionPercent = $Reduction
        }
    }
}
$ReportPath = Join-Path $ReportRoot (
    "MultiplayerNetworkTest-{0}.json" -f (Get-Date).ToString("yyyyMMdd-HHmmss"))
$Summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8

if ($PublishEvidence) {
    $EvidenceRoot = Join-Path $ProjectRoot "Tests\Evidence"
    New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null
    $Commit = (git -C $ProjectRoot rev-parse --short HEAD 2>$null)
    $SourceFilesModified = [bool](git -C $ProjectRoot status --porcelain -- Source Scripts Config "*.bat" "*.uproject" 2>$null)
    $SourceFingerprint = Get-SourceFingerprint
    $PublicResults = @($Results | ForEach-Object {
        [ordered]@{
            Profile = $_.Profile
            LagMs = $_.LagMs
            LagVarianceMs = $_.LagVarianceMs
            LossPercent = $_.LossPercent
            ReconnectTested = $_.ReconnectTested
            SessionTested = $_.SessionTested
            GameplayFlowTested = $_.GameplayFlowTested
            RestartAndLeaveTested = $_.RestartAndLeaveTested
            BandwidthProfile = $_.BandwidthProfile
            BandwidthSample = $_.BandwidthSample
            Success = $_.Success
            DurationSeconds = $_.DurationSeconds
            Evidence = $_.Evidence
            Error = $_.Error
        }
    })
    $PublicSummary = [ordered]@{
        SchemaVersion = 1
        GeneratedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
        GitBaseCommit = $Commit
        SourceFilesModifiedAtTestTime = $SourceFilesModified
        SourceFingerprintSha256 = $SourceFingerprint
        Engine = "Unreal Engine 5.5"
        Environment = "Two local UnrealEditor processes using OnlineSubsystemNull"
        Success = $Summary.Success
        Results = $PublicResults
        BandwidthComparison = $Summary.BandwidthComparison
    }
    $EvidencePath = Join-Path $EvidenceRoot ($EvidenceName + ".json")
    $PublicSummary | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $EvidencePath -Encoding UTF8
    Write-Host "Public evidence: $EvidencePath"
}

Write-Host "Report: $ReportPath"
if ($Failures.Count -gt 0) {
    exit 1
}
exit 0
