[CmdletBinding()]
param(
    [ValidateSet("Normal", "Moderate", "Harsh", "All")]
    [string]$Profile = "Normal",

    [switch]$TestReconnect,

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

    $Process.Refresh()
    if (-not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        Wait-Process -Id $Process.Id -Timeout 10 -ErrorAction SilentlyContinue | Out-Null
    }
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
        [string]$ReconnectClientLog = ""
    )

    foreach ($Path in $Paths) {
        $Text = Read-Log -Path $Path
        if ($Path -eq $ReconnectClientLog) {
            $Text = [regex]::Replace(
                $Text,
                "(?im)^.*Development reconnect test.*(?:\r?\n)?",
                "")
        }

        $FailurePattern =
            "(?im)(Fatal error:|Ensure condition failed|LogMultiplayer:\s+Error:|LogNet:\s+Error:|NetworkFailure:|TravelFailure:)"
        $Failure = [regex]::Match($Text, $FailurePattern)
        if ($Failure.Success) {
            throw "Runtime failure marker '$($Failure.Value)' was found in $Path."
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
        [int]$TestPort
    )

    $StartedAt = Get-Date
    $RunId = "{0}-{1}" -f $StartedAt.ToString("yyyyMMdd-HHmmss-fff"), $NetworkProfile.Name
    $HostLog = Join-Path $LogRoot "$RunId-Host.log"
    $ClientLog = Join-Path $LogRoot "$RunId-Client.log"
    $Processes = @()
    $Evidence = @()
    $ErrorMessage = $null
    $Success = $false

    try {
        Write-Host "[$($NetworkProfile.Name)] Starting listen server on port $TestPort..."
        $HostProcess = Start-Game -Address ($TestMap + "?listen") -LogPath $HostLog -NetworkProfile $NetworkProfile -TestPort $TestPort -IsHost
        $Processes += $HostProcess
        Wait-ForLog -Path $HostLog -Pattern "Coop objective configured: RequiredKeys=4" -ExpectedCount 1 -Process $HostProcess -Description "Listen server startup"
        $Evidence += "Server initialized RequiredKeys=4"

        Write-Host "[$($NetworkProfile.Name)] Starting client..."
        $ClientArguments = if ($TestReconnect) { @("-CoopTestReconnect") } else { @() }
        $ClientProcess = Start-Game -Address "127.0.0.1:$TestPort" -LogPath $ClientLog -NetworkProfile $NetworkProfile -TestPort $TestPort -ExtraArguments $ClientArguments
        $Processes += $ClientProcess
        Wait-ForLog -Path $ClientLog -Pattern "Welcomed by server \(Level: /Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo" -ExpectedCount 1 -Process $ClientProcess -Description "Client connection"
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
            $Evidence += "The same client process automatically reconnected after a simulated connection loss"
        }

        $ExpectedDisconnectLog = if ($TestReconnect) { $ClientLog } else { "" }
        Assert-Logs -Paths $Logs -NetworkProfile $NetworkProfile -ReconnectClientLog $ExpectedDisconnectLog
        $Success = $true
    }
    catch {
        $ErrorMessage = $_.Exception.Message
    }
    finally {
        foreach ($Process in $Processes) {
            Stop-Game -Process $Process
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

New-Item -ItemType Directory -Force -Path $LogRoot | Out-Null
New-Item -ItemType Directory -Force -Path $ReportRoot | Out-Null

if ($Profile -eq "All") {
    [object[]]$ProfilesToRun = @($Profiles.Values)
}
else {
    [object[]]$ProfilesToRun = @($Profiles[$Profile])
}

$Results = @()
for ($Index = 0; $Index -lt $ProfilesToRun.Count; ++$Index) {
    $Result = Invoke-NetworkTest -NetworkProfile $ProfilesToRun[$Index] -TestPort ($Port + $Index)
    $Results += $Result
    if ($Result.Success) {
        Write-Host "[$($Result.Profile)] PASS in $($Result.DurationSeconds)s" -ForegroundColor Green
    }
    else {
        Write-Host "[$($Result.Profile)] FAIL: $($Result.Error)" -ForegroundColor Red
    }
}

$Failures = @($Results | Where-Object { -not $_.Success })
$Summary = [ordered]@{
    Project = $ProjectPath
    Editor = $EditorPath
    RequestedProfile = $Profile
    ReconnectTested = [bool]$TestReconnect
    Success = $Failures.Count -eq 0
    Results = $Results
}
$ReportPath = Join-Path $ReportRoot (
    "MultiplayerNetworkTest-{0}.json" -f (Get-Date).ToString("yyyyMMdd-HHmmss"))
$Summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ReportPath -Encoding UTF8

Write-Host "Report: $ReportPath"
if ($Failures.Count -gt 0) {
    exit 1
}
exit 0
