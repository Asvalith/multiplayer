param(
    [string]$RunId
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $PSScriptRoot
$runsDir = Join-Path $projectDir 'Saved\GASBaseline'

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $latestRun = Get-ChildItem -LiteralPath $runsDir -Directory |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latestRun) {
        throw "No GAS baseline run exists under $runsDir"
    }
    $RunId = $latestRun.Name
}

$runDir = Join-Path $runsDir $RunId
$hostLogPath = Join-Path $runDir 'Host.log'
$clientLogPath = Join-Path $runDir 'Client.log'
$runInfoPath = Join-Path $runDir 'RunInfo.txt'

foreach ($requiredPath in @($hostLogPath, $clientLogPath, $runInfoPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required evidence file is missing: $requiredPath"
    }
}

$hostLog = Get-Content -LiteralPath $hostLogPath -Raw
$clientLog = Get-Content -LiteralPath $clientLogPath -Raw
$runInfo = Get-Content -LiteralPath $runInfoPath -Raw
$expectedLagMatch = [regex]::Match($runInfo, '(?m)^HostOutgoingPktLagMs=(\d+)\r?$')
$expectedLag = if ($expectedLagMatch.Success) { [int]$expectedLagMatch.Groups[1].Value } else { $null }
$expectedLossMatch = [regex]::Match($runInfo, '(?m)^HostOutgoingPktLossPercent=(\d+)\r?$')
$expectedLoss = if ($expectedLossMatch.Success) { [int]$expectedLossMatch.Groups[1].Value } else { $null }

function Get-MatchCount {
    param(
        [string]$Text,
        [string]$Pattern
    )

    return [regex]::Matches($Text, $Pattern).Count
}

function Get-KeyedLogEvents {
    param(
        [string]$Text,
        [string]$Pattern
    )

    $events = @()
    foreach ($match in [regex]::Matches($Text, $Pattern)) {
        $events += [pscustomobject]@{
            PredictionKey = [int]$match.Groups['key'].Value
            TimestampText = $match.Groups['timestamp'].Value
            Timestamp = [datetime]::ParseExact(
                $match.Groups['timestamp'].Value,
                'yyyy.MM.dd-HH.mm.ss:fff',
                [Globalization.CultureInfo]::InvariantCulture)
        }
    }
    return $events
}

function Get-ProcessSummary {
    param(
        [string]$Name,
        [string]$Text
    )

    [ordered]@{
        Process = $Name
        CueEmit = Get-MatchCount $Text 'GAS_CUE_EMIT'
        CueHandler = Get-MatchCount $Text 'GAS_CUE_HANDLER'
        TargetTraceSelected = Get-MatchCount $Text 'GAS_TARGET_TRACE Phase=Selected'
        AutoSequenceComplete = Get-MatchCount $Text 'GAS_M5_AUTO Phase=SequenceComplete'
        DamageCastExecuted = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.Damage\.Cast Event=EGameplayCueEvent::Executed'
        DamageImpactExecuted = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.Damage\.Impact Event=EGameplayCueEvent::Executed'
        DummyDamageImpactExecuted = Get-MatchCount $Text 'Actor=multiplayerGASTargetDummy[^\r\n]*Cue=GameplayCue\.Coop\.Damage\.Impact Event=EGameplayCueEvent::Executed'
        CharacterDamageImpactExecuted = Get-MatchCount $Text 'Actor=BP_ThirdPersonCharacter[^\r\n]*Cue=GameplayCue\.Coop\.Damage\.Impact Event=EGameplayCueEvent::Executed'
        CriticalImpact = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.Damage\.Impact[^\r\n]*Critical=true'
        HealCastExecuted = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.Heal\.Cast Event=EGameplayCueEvent::Executed'
        HealResultExecuted = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.Heal\.Result Event=EGameplayCueEvent::Executed'
        ImmunityOnActive = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.State\.Immunity Event=EGameplayCueEvent::OnActive'
        ImmunityWhileActive = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.State\.Immunity Event=EGameplayCueEvent::WhileActive'
        ImmunityRemoved = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.State\.Immunity Event=EGameplayCueEvent::Removed'
        VulnerabilityOnActive = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.State\.Vulnerability Event=EGameplayCueEvent::OnActive'
        VulnerabilityWhileActive = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.State\.Vulnerability Event=EGameplayCueEvent::WhileActive'
        VulnerabilityRemoved = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.State\.Vulnerability Event=EGameplayCueEvent::Removed'
        DeathExecuted = Get-MatchCount $Text 'Cue=GameplayCue\.Coop\.Death Event=EGameplayCueEvent::Executed'
        PredictionCaughtUp = Get-MatchCount $Text 'GAS_PREDICTION Phase=CaughtUp'
        PredictionRejected = Get-MatchCount $Text 'GAS_PREDICTION Phase=Rejected'
        FatalOrEnsure = Get-MatchCount $Text '(?im)(Fatal error:|=== Critical error:|Ensure condition failed:)'
    }
}

$hostSummary = Get-ProcessSummary 'Host' $hostLog
$clientSummary = Get-ProcessSummary 'Client' $clientLog
$clientPredictedDamage = @(Get-KeyedLogEvents $clientLog '\[(?<timestamp>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\][^\r\n]*GAS_CUE_EMIT Phase=PredictEmit Ability=Damage[^\r\n]*PredictionKey=\[(?<key>\d+)/')
$hostAuthorityDamage = @(Get-KeyedLogEvents $hostLog '\[(?<timestamp>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\][^\r\n]*GAS_CUE_EMIT Phase=AuthorityEmit Ability=Damage[^\r\n]*PredictionKey=\[(?<key>\d+)/')
$clientCaughtUpDamage = @(Get-KeyedLogEvents $clientLog '\[(?<timestamp>\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\][^\r\n]*GAS_PREDICTION Phase=CaughtUp Ability=Damage PredictionKey=(?<key>\d+)')

$damagePredictionTimings = @()
foreach ($prediction in $clientPredictedDamage) {
    $authority = $hostAuthorityDamage |
        Where-Object PredictionKey -eq $prediction.PredictionKey |
        Select-Object -First 1
    $caughtUp = $clientCaughtUpDamage |
        Where-Object PredictionKey -eq $prediction.PredictionKey |
        Select-Object -First 1

    $damagePredictionTimings += [ordered]@{
        PredictionKey = $prediction.PredictionKey
        ClientPredict = $prediction.TimestampText
        ServerAuthority = if ($null -ne $authority) { $authority.TimestampText } else { $null }
        ClientCaughtUp = if ($null -ne $caughtUp) { $caughtUp.TimestampText } else { $null }
        PredictLeadOverAuthorityMs = if ($null -ne $authority) {
            [math]::Round(($authority.Timestamp - $prediction.Timestamp).TotalMilliseconds, 1)
        } else { $null }
        CatchUpAfterPredictMs = if ($null -ne $caughtUp) {
            [math]::Round(($caughtUp.Timestamp - $prediction.Timestamp).TotalMilliseconds, 1)
        } else { $null }
    }
}

$metadata = [ordered]@{
    RunId = $RunId
    HostReady = $runInfo -match '(?m)^HostReady=true\r?$'
    ClientJoined = ($runInfo -match '(?m)^ClientJoined=true\r?$') -and ($hostLog -match 'Join succeeded:')
    HostLagArgumentPresent = $hostLog -match '(?i)-PktLag='
    ClientLagArgumentPresent = $clientLog -match '(?i)-PktLag='
    HostLagApplied = $null -ne $expectedLag -and $hostLog -match "PktLag set to $expectedLag"
    ClientLagApplied = $null -ne $expectedLag -and $clientLog -match "PktLag set to $expectedLag"
    HostLossApplied = $null -ne $expectedLoss -and $hostLog -match "PktLoss set to $expectedLoss"
    ClientLossApplied = $null -ne $expectedLoss -and $clientLog -match "PktLoss set to $expectedLoss"
    NoFatalOrEnsure = ($hostSummary.FatalOrEnsure + $clientSummary.FatalOrEnsure) -eq 0
    AutoSequenceComplete = $clientSummary.AutoSequenceComplete -eq 1
    DamagePredictionPairs = $damagePredictionTimings.Count
    EvidenceBoundary = 'Counts summarize an executed two-process run; they do not by themselves prove PredictionKey identity or visual quality.'
}

$result = [ordered]@{
    Metadata = $metadata
    Processes = @($hostSummary, $clientSummary)
    DamagePredictionTimings = $damagePredictionTimings
}

$jsonPath = Join-Path $runDir 'M5Summary.json'
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding utf8

$markdown = @(
    '# GAS M5 log summary'
    ''
    "- Run: ``$RunId``"
    "- Host ready: ``$($metadata.HostReady)``"
    "- Client joined: ``$($metadata.ClientJoined)``"
    "- Lag argument present (Host / Client): ``$($metadata.HostLagArgumentPresent) / $($metadata.ClientLagArgumentPresent)``"
    "- Lag applied in NetDriver (Host / Client): ``$($metadata.HostLagApplied) / $($metadata.ClientLagApplied)``"
    "- Loss applied in NetDriver (Host / Client): ``$($metadata.HostLossApplied) / $($metadata.ClientLossApplied)``"
    "- No Fatal/Ensure: ``$($metadata.NoFatalOrEnsure)``"
    "- Auto sequence complete: ``$($metadata.AutoSequenceComplete)``"
    ''
    '| Process | Target selected | Emit | Handler | Damage Cast | Dummy/Character Impact | Critical | Heal C/R | Immunity A/W/R | Vulnerability A/W/R | Death | CatchUp | Reject |'
    '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|'
)

foreach ($process in @($hostSummary, $clientSummary)) {
    $markdown += "| $($process.Process) | $($process.TargetTraceSelected) | $($process.CueEmit) | $($process.CueHandler) | $($process.DamageCastExecuted) | $($process.DummyDamageImpactExecuted)/$($process.CharacterDamageImpactExecuted) | $($process.CriticalImpact) | $($process.HealCastExecuted)/$($process.HealResultExecuted) | $($process.ImmunityOnActive)/$($process.ImmunityWhileActive)/$($process.ImmunityRemoved) | $($process.VulnerabilityOnActive)/$($process.VulnerabilityWhileActive)/$($process.VulnerabilityRemoved) | $($process.DeathExecuted) | $($process.PredictionCaughtUp) | $($process.PredictionRejected) |"
}

$markdown += ''
$markdown += '| Damage PredictionKey | Client predict | Server authority | Client catch-up | Predict lead over authority (ms) | Catch-up after predict (ms) |'
$markdown += '|---:|---|---|---|---:|---:|'
foreach ($timing in $damagePredictionTimings) {
    $markdown += "| $($timing.PredictionKey) | $($timing.ClientPredict) | $($timing.ServerAuthority) | $($timing.ClientCaughtUp) | $($timing.PredictLeadOverAuthorityMs) | $($timing.CatchUpAfterPredictMs) |"
}

$markdown += ''
$markdown += '> This is an offline inventory of a run that actually occurred. Cross-process timing uses the same local machine clock. Manual visual review is still required for Niagara/audio quality; this run uses PointLight placeholders.'

$markdownPath = Join-Path $runDir 'M5Summary.md'
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8

Write-Output "M5 summaries written:"
Write-Output $markdownPath
Write-Output $jsonPath
$result | ConvertTo-Json -Depth 5
