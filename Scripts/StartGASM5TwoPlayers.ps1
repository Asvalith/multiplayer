param(
    [ValidateRange(0, 5000)]
    [int]$PktLagMs = 0,

    [ValidateRange(0, 100)]
    [int]$PktLossPercent = 0,

    [ValidateSet('M5', 'M6', 'M6Intent')]
    [string]$Stage = 'M5',

    [switch]$AutoSequence,

    [switch]$Headless,

    [ValidateRange(1, 65535)]
    [int]$Port = 17777,

    [ValidateRange(10, 180)]
    [int]$HostReadyTimeoutSeconds = 60,

    [ValidateRange(10, 180)]
    [int]$ClientJoinTimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $PSScriptRoot
$projectPath = Join-Path $projectDir 'multiplayer.uproject'
$editorPath = 'E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe'
$testMap = '/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo'
$logCommands = 'LogMultiplayerGAS VeryVerbose,LogNet Log,LogGameplayCues VeryVerbose,LogAbilitySystem Verbose'

foreach ($requiredPath in @($projectPath, $editorPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required path is missing: $requiredPath"
    }
}

$existingListener = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($null -ne $existingListener) {
    throw "Port $Port is already in use by PID $($existingListener[0].OwningProcess)."
}

$runId = Get-Date -Format 'yyyyMMdd_HHmmss'
$runDir = Join-Path $projectDir "Saved\GASBaseline\$runId"
$hostLogPath = Join-Path $runDir 'Host.log'
$clientLogPath = Join-Path $runDir 'Client.log'
$runInfoPath = Join-Path $runDir 'RunInfo.txt'
New-Item -ItemType Directory -Path $runDir -Force | Out-Null

$stageArguments = if ($Stage -eq 'M6') {
    @('-GASM6Lab')
} elseif ($Stage -eq 'M6Intent') {
    @('-GASM6IntentLab')
} else {
    @()
}
$autoArgument = if ($AutoSequence) {
    if ($Stage -eq 'M6') {
        @('-GASM6Auto')
    } elseif ($Stage -eq 'M6Intent') {
        @('-GASM6IntentAuto')
    } else {
        @('-GASM5Auto')
    }
} else { @() }
$displayArguments = if ($Headless) {
    @('-NullRHI', '-Unattended', '-NoSound')
} else {
    @('-windowed', '-ResX=900', '-ResY=650', '-WinX=0', '-WinY=40')
}
$clientDisplayArguments = if ($Headless) {
    @('-NullRHI', '-Unattended', '-NoSound')
} else {
    @('-windowed', '-ResX=900', '-ResY=650', '-WinX=900', '-WinY=40')
}
$stageNote = if ($Stage -eq 'M6') {
    'M6 uses a non-Shipping server-only one-shot Immunity activation rejection lab'
} elseif ($Stage -eq 'M6Intent') {
    'M6Intent uses non-Shipping client payload mutations and the real GAS TargetData RPC; verdict RPCs never modify gameplay state'
} else {
    'M5 exercises the GameplayCue prediction acceptance and reconciliation path'
}
$hostArguments = @(
    $projectPath,
    "$testMap`?listen",
    '-game'
) + $displayArguments + $stageArguments + @(
    "-port=$Port", '-NoSplash', '-ForceLogFlush',
    "-PktLag=$PktLagMs", "-PktLoss=$PktLossPercent",
    "-AbsLog=$hostLogPath",
    "-LogCmds=`"$logCommands`"",
    '-DDC=InstalledNoZenLocalFallback'
)
$clientArguments = @(
    $projectPath,
    "127.0.0.1:$Port",
    '-game'
) + $clientDisplayArguments + $stageArguments + @(
    '-NoSplash', '-ForceLogFlush',
    "-PktLag=$PktLagMs", "-PktLoss=$PktLossPercent"
) + $autoArgument + @(
    "-AbsLog=$clientLogPath",
    "-LogCmds=`"$logCommands`"",
    '-DDC=InstalledNoZenLocalFallback'
)

@(
    "RunId=$runId"
    "TestMap=$testMap"
    "Port=$Port"
    'Window=900x650'
    "Headless=$([bool]$Headless)"
    "Stage=$Stage"
    "HostOutgoingPktLagMs=$PktLagMs"
    "ClientOutgoingPktLagMs=$PktLagMs"
    "ApproxRoundTripLagMs=$($PktLagMs * 2)"
    "HostOutgoingPktLossPercent=$PktLossPercent"
    "ClientOutgoingPktLossPercent=$PktLossPercent"
    "NetworkConditionNote=PktLag applies in each direction; $PktLagMs on both peers is approximately $($PktLagMs * 2)ms RTT"
    "ClientAutoSequence=$([bool]$AutoSequence)"
    'FormalControls=LMB Damage Enemy, Q Self Heal, E Immunity'
    'DebugControls=1 Network/GAS Status, 4 Damage Enemy, 5 Self Heal, 6 Immunity, 7 Spawn/Reset Enemy Target, 8 Enemy Damages Self, 9 Arm M6 Immunity Reject Lab'
    'ExpectedPlayerState=multiplayerGASPlayerState'
    "StageLabNote=$stageNote"
) | Set-Content -LiteralPath $runInfoPath -Encoding utf8

function Wait-ForLogPattern {
    param(
        [string]$Path,
        [string]$Pattern,
        [int]$TimeoutSeconds,
        [System.Diagnostics.Process]$Process
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $Process.Refresh()
        if ($Process.HasExited) {
            throw "Process $($Process.Id) exited before log pattern appeared: $Pattern"
        }
        if (Test-Path -LiteralPath $Path) {
            $text = Get-Content -LiteralPath $Path -Raw
            if ($text -match $Pattern) {
                return
            }
        }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    throw "Timed out waiting for '$Pattern' in $Path"
}

$hostProcess = Start-Process -FilePath $editorPath -ArgumentList $hostArguments -PassThru
Add-Content -LiteralPath $runInfoPath -Value "HostPid=$($hostProcess.Id)"

try {
    Wait-ForLogPattern `
        -Path $hostLogPath `
        -Pattern "IpNetDriver listening on port $Port" `
        -TimeoutSeconds $HostReadyTimeoutSeconds `
        -Process $hostProcess
    Add-Content -LiteralPath $runInfoPath -Value 'HostReady=true'

    $clientProcess = Start-Process -FilePath $editorPath -ArgumentList $clientArguments -PassThru
    Add-Content -LiteralPath $runInfoPath -Value "ClientPid=$($clientProcess.Id)"

    Wait-ForLogPattern `
        -Path $hostLogPath `
        -Pattern 'Join succeeded:' `
        -TimeoutSeconds $ClientJoinTimeoutSeconds `
        -Process $clientProcess
    Add-Content -LiteralPath $runInfoPath -Value 'ClientJoined=true'
}
catch {
    Add-Content -LiteralPath $runInfoPath -Value "LaunchError=$($_.Exception.Message)"
    throw
}

[pscustomobject]@{
    RunId = $runId
    RunDirectory = $runDir
    HostPid = $hostProcess.Id
    ClientPid = $clientProcess.Id
    PktLagPerDirectionMs = $PktLagMs
    ApproxRoundTripLagMs = $PktLagMs * 2
    PktLossPercentPerDirection = $PktLossPercent
    AutoSequence = [bool]$AutoSequence
    Headless = [bool]$Headless
    Stage = $Stage
}
