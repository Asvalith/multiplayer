param(
    [string]$RunId = '',

    [string]$BaselineRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($BaselineRoot)) {
    $projectDir = Split-Path -Parent $PSScriptRoot
    $BaselineRoot = Join-Path $projectDir 'Saved\GASBaseline'
}

if (-not (Test-Path -LiteralPath $BaselineRoot)) {
    throw "GAS baseline root does not exist: $BaselineRoot"
}

function Read-KeyValueFile {
    param([string]$Path)

    $result = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        $match = [regex]::Match($line, '^(?<key>[^=]+)=(?<value>.*)$')
        if ($match.Success) {
            $result[$match.Groups['key'].Value] = $match.Groups['value'].Value
        }
    }
    return $result
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $candidateDirectories = Get-ChildItem -LiteralPath $BaselineRoot -Directory |
        Sort-Object LastWriteTime -Descending
    foreach ($candidate in $candidateDirectories) {
        $candidateInfoPath = Join-Path $candidate.FullName 'RunInfo.txt'
        if (-not (Test-Path -LiteralPath $candidateInfoPath)) {
            continue
        }
        $candidateInfo = Read-KeyValueFile -Path $candidateInfoPath
        if ($candidateInfo.ContainsKey('Stage') -and $candidateInfo['Stage'] -eq 'M6') {
            $RunId = $candidate.Name
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    throw 'No Stage=M6 run was found.'
}

$runDirectory = Join-Path $BaselineRoot $RunId
$runInfoPath = Join-Path $runDirectory 'RunInfo.txt'
$hostLogPath = Join-Path $runDirectory 'Host.log'
$clientLogPath = Join-Path $runDirectory 'Client.log'

foreach ($requiredPath in @($runInfoPath, $hostLogPath, $clientLogPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "M6 evidence file is missing: $requiredPath"
    }
}

$runInfo = Read-KeyValueFile -Path $runInfoPath
$hostLog = Get-Content -LiteralPath $hostLogPath -Raw
$clientLog = Get-Content -LiteralPath $clientLogPath -Raw
$script:checks = @()

function Add-Check {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Details
    )

    $script:checks += [pscustomobject]@{
        Name = $Name
        Passed = $Passed
        Details = $Details
    }
}

function Get-Matches {
    param(
        [string]$Text,
        [string]$Pattern
    )

    return ,@([regex]::Matches(
        $Text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline))
}

function Add-ExactCountCheck {
    param(
        [string]$Name,
        [System.Text.RegularExpressions.Match[]]$Matches,
        [int]$Expected
    )

    Add-Check `
        -Name $Name `
        -Passed ($Matches.Count -eq $Expected) `
        -Details "Expected=$Expected Actual=$($Matches.Count)"
}

function Convert-LineToFields {
    param([string]$Line)

    $fields = @{}
    foreach ($fieldMatch in [regex]::Matches($Line, '(?<key>[A-Za-z][A-Za-z0-9]*)=(?<value>[^\s]+)')) {
        $fields[$fieldMatch.Groups['key'].Value] = $fieldMatch.Groups['value'].Value
    }
    return $fields
}

function Add-FieldCheck {
    param(
        [string]$Name,
        [hashtable]$Fields,
        [string]$Field,
        [string]$Expected
    )

    $actual = if ($Fields.ContainsKey($Field)) { $Fields[$Field] } else { '<missing>' }
    Add-Check `
        -Name $Name `
        -Passed ($actual -eq $Expected) `
        -Details "$Field Expected=$Expected Actual=$actual"
}

function Test-StrictlyIncreasing {
    param([int[]]$Values)

    for ($index = 1; $index -lt $Values.Count; ++$index) {
        if ($Values[$index] -le $Values[$index - 1]) {
            return $false
        }
    }
    return $true
}

Add-Check -Name 'RunInfo Stage' -Passed (
    $runInfo.ContainsKey('Stage') -and $runInfo['Stage'] -eq 'M6') -Details "Stage=$($runInfo['Stage'])"
Add-Check -Name 'RunInfo AutoSequence' -Passed (
    $runInfo.ContainsKey('ClientAutoSequence') -and $runInfo['ClientAutoSequence'] -eq 'True') -Details "ClientAutoSequence=$($runInfo['ClientAutoSequence'])"
Add-Check -Name 'Host reached listen state' -Passed (
    $runInfo.ContainsKey('HostReady') -and $runInfo['HostReady'] -eq 'true') -Details "HostReady=$($runInfo['HostReady'])"
Add-Check -Name 'Client joined host' -Passed (
    $runInfo.ContainsKey('ClientJoined') -and $runInfo['ClientJoined'] -eq 'true') -Details "ClientJoined=$($runInfo['ClientJoined'])"

$lag = if ($runInfo.ContainsKey('HostOutgoingPktLagMs')) { $runInfo['HostOutgoingPktLagMs'] } else { '<missing>' }
$loss = if ($runInfo.ContainsKey('HostOutgoingPktLossPercent')) { $runInfo['HostOutgoingPktLossPercent'] } else { '<missing>' }
Add-Check -Name 'Host lag injection was applied' -Passed (
    $hostLog -match "PktLag set to $([regex]::Escape($lag))") -Details "PktLag=$lag"
Add-Check -Name 'Client lag injection was applied' -Passed (
    $clientLog -match "PktLag set to $([regex]::Escape($lag))") -Details "PktLag=$lag"
Add-Check -Name 'Host loss injection was applied' -Passed (
    $hostLog -match "PktLoss set to $([regex]::Escape($loss))") -Details "PktLoss=$loss"
Add-Check -Name 'Client loss injection was applied' -Passed (
    $clientLog -match "PktLoss set to $([regex]::Escape($loss))") -Details "PktLoss=$loss"

$severePattern = '(?im)Fatal error:|Critical error:|Ensure condition failed'
Add-Check -Name 'Host has no fatal/assert/ensure' -Passed (-not [regex]::IsMatch($hostLog, $severePattern)) -Details 'Fatal/Critical/Ensure scan'
Add-Check -Name 'Client has no fatal/assert/ensure' -Passed (-not [regex]::IsMatch($clientLog, $severePattern)) -Details 'Fatal/Critical/Ensure scan'

$hostArmed = Get-Matches $hostLog 'GAS_M6_REJECT Phase=AuthorityArmed TrialId=(?<trial>[0-9]+) Ability=Immunity[^\r\n]*'
$hostArmResult = Get-Matches $hostLog 'GAS_M6_REJECT Phase=ArmResult TrialId=(?<trial>[0-9]+) Ability=Immunity[^\r\n]*Armed=true'
$hostRejected = Get-Matches $hostLog 'GAS_M6_REJECT Phase=AuthorityRejected TrialId=(?<trial>[0-9]+) Ability=Immunity[^\r\n]*Spec=(?<spec>[0-9]+) PredictionKey=\[(?<key>-?[0-9]+)\/(?<base>-?[0-9]+)\] PredictionKeyCurrent=(?<current>-?[0-9]+) TagRemaining=(?<remaining>[0-9]+)'
$hostAuthorityCommitted = Get-Matches $hostLog 'GAS_M6_STATE Phase=AuthorityCommitted TrialId=(?<trial>[0-9]+) Ability=Immunity Spec=(?<spec>[0-9]+) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*'

$clientArmConfirmed = Get-Matches $clientLog 'GAS_M6_REJECT Phase=ClientArmConfirmed TrialId=(?<trial>[0-9]+) Ability=Immunity Armed=true'
$clientForcedInput = Get-Matches $clientLog 'GAS_M6_AUTO Phase=ForcedRejectInput TrialId=(?<trial>[0-9]+) Ability=Immunity'
$clientRecoveryInput = Get-Matches $clientLog 'GAS_M6_AUTO Phase=RecoveryInput TrialId=(?<trial>[0-9]+) Ability=Immunity'
$clientPass = Get-Matches $clientLog 'GAS_M6_AUTO Phase=SequenceComplete Result=Pass TrialId=(?<trial>[0-9]+) Ability=Immunity'
$clientFail = Get-Matches $clientLog 'GAS_M6_AUTO Phase=SequenceComplete Result=Fail[^\r\n]*'
$clientPredicted = Get-Matches $clientLog 'GAS_M6_STATE Phase=PredictedCommitted TrialId=(?<trial>[0-9]+) Ability=Immunity Spec=(?<spec>[0-9]+) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*'
$clientRejected = Get-Matches $clientLog 'GAS_PREDICTION Phase=Rejected Ability=Immunity Spec=(?<spec>[0-9]+) ActivationKey=(?<activation>-?[0-9]+) ActionKey=(?<key>-?[0-9]+) ActionBaseKey=(?<base>-?[0-9]+)[^\r\n]*'
$clientPostRejectCatchUp = Get-Matches $clientLog 'GAS_PREDICTION Phase=PostRejectCatchUp Ability=Immunity Spec=(?<spec>[0-9]+) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*'
$clientAcceptedCatchUp = Get-Matches $clientLog 'GAS_PREDICTION Phase=CaughtUp Ability=Immunity Spec=(?<spec>[0-9]+) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*'
$clientEngineReject = Get-Matches $clientLog 'ClientActivateAbilityFailed_Implementation\. PredictionKey\s*:(?<key>-?[0-9]+) Ability:\s*(?<ability>\S+)'
$clientRejectedState = Get-Matches $clientLog 'GAS_M6_STATE Phase=RejectedReconciled TrialId=(?<trial>[0-9]+) Ability=Immunity Spec=(?<spec>[0-9]+) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*'
$clientAcceptedState = Get-Matches $clientLog 'GAS_M6_STATE Phase=(?:Accepted|Reconciled)CaughtUp TrialId=(?<trial>[0-9]+) Ability=Immunity Spec=(?<spec>[0-9]+) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*'
$clientRejectedVisual = Get-Matches $clientLog 'GAS_M6_VISUAL Phase=PendingCleared Outcome=Rejected PredictionKey=(?<key>-?[0-9]+)[^\r\n]*PendingVisual=false'
$clientAcceptedVisual = Get-Matches $clientLog 'GAS_M6_VISUAL Phase=PendingCleared Outcome=(?:Accepted|CaughtUp) PredictionKey=(?<key>-?[0-9]+)[^\r\n]*PendingVisual=false'
$postRejectSnapshot = Get-Matches $clientLog 'GAS_M6_SNAPSHOT Phase=PostRejectCheckpoint[^\r\n]*'
$postRecoverySnapshot = Get-Matches $clientLog 'GAS_M6_SNAPSHOT Phase=PostRecoveryCheckpoint[^\r\n]*'

foreach ($countCheck in @(
    [pscustomobject]@{ Name = 'Authority armed once'; Matches = $hostArmed; Expected = 1 },
    [pscustomobject]@{ Name = 'Authority arm result succeeded once'; Matches = $hostArmResult; Expected = 1 },
    [pscustomobject]@{ Name = 'Authority rejected once'; Matches = $hostRejected; Expected = 1 },
    [pscustomobject]@{ Name = 'Authority accepted recovery once'; Matches = $hostAuthorityCommitted; Expected = 1 },
    [pscustomobject]@{ Name = 'Client arm acknowledgement once'; Matches = $clientArmConfirmed; Expected = 1 },
    [pscustomobject]@{ Name = 'Forced reject input once'; Matches = $clientForcedInput; Expected = 1 },
    [pscustomobject]@{ Name = 'Recovery input once'; Matches = $clientRecoveryInput; Expected = 1 },
    [pscustomobject]@{ Name = 'Client predicted exactly two activations'; Matches = $clientPredicted; Expected = 2 },
    [pscustomobject]@{ Name = 'Client rejection callback once'; Matches = $clientRejected; Expected = 1 },
    [pscustomobject]@{ Name = 'Engine ClientActivateAbilityFailed once'; Matches = $clientEngineReject; Expected = 1 },
    [pscustomobject]@{ Name = 'Rejected state snapshot once'; Matches = $clientRejectedState; Expected = 1 },
    [pscustomobject]@{ Name = 'Accepted CatchUp once'; Matches = $clientAcceptedCatchUp; Expected = 1 },
    [pscustomobject]@{ Name = 'Accepted state snapshot once'; Matches = $clientAcceptedState; Expected = 1 },
    [pscustomobject]@{ Name = 'Rejected pending visual cleared once'; Matches = $clientRejectedVisual; Expected = 1 },
    [pscustomobject]@{ Name = 'Accepted pending visual cleared once'; Matches = $clientAcceptedVisual; Expected = 1 },
    [pscustomobject]@{ Name = 'Post-reject checkpoint once'; Matches = $postRejectSnapshot; Expected = 1 },
    [pscustomobject]@{ Name = 'Post-recovery checkpoint once'; Matches = $postRecoverySnapshot; Expected = 1 },
    [pscustomobject]@{ Name = 'Sequence pass once'; Matches = $clientPass; Expected = 1 },
    [pscustomobject]@{ Name = 'Sequence has no failure'; Matches = $clientFail; Expected = 0 }
)) {
    Add-ExactCountCheck -Name $countCheck.Name -Matches @($countCheck.Matches) -Expected $countCheck.Expected
}

if ($hostRejected.Count -eq 1 -and $clientRejected.Count -eq 1 -and
    $clientEngineReject.Count -eq 1 -and $clientPredicted.Count -eq 2 -and
    $hostAuthorityCommitted.Count -eq 1 -and $clientAcceptedCatchUp.Count -eq 1) {
    $trial = $hostRejected[0].Groups['trial'].Value
    $spec = $hostRejected[0].Groups['spec'].Value
    $rejectedKey = $hostRejected[0].Groups['key'].Value
    $recoveryKey = $clientAcceptedCatchUp[0].Groups['key'].Value

    Add-Check -Name 'Rejected key numeric field matches serialized key' -Passed (
        $hostRejected[0].Groups['current'].Value -eq $rejectedKey) -Details "Key=$rejectedKey"
    Add-Check -Name 'Forced rejection consumed its server-only tag' -Passed (
        $hostRejected[0].Groups['remaining'].Value -eq '0') -Details "TagRemaining=$($hostRejected[0].Groups['remaining'].Value)"
    Add-Check -Name 'Rejected activation uses one prediction key' -Passed (
        $clientPredicted[0].Groups['key'].Value -eq $rejectedKey -and
        $clientRejected[0].Groups['activation'].Value -eq $rejectedKey -and
        $clientRejected[0].Groups['key'].Value -eq $rejectedKey -and
        $clientEngineReject[0].Groups['key'].Value -eq $rejectedKey) -Details "RejectedKey=$rejectedKey"
    Add-Check -Name 'Rejected key did not become an accepted authority commit' -Passed (
        $hostAuthorityCommitted[0].Groups['key'].Value -ne $rejectedKey) -Details "Rejected=$rejectedKey AuthorityAccepted=$($hostAuthorityCommitted[0].Groups['key'].Value)"
    Add-Check -Name 'Rejected key did not become normal client acceptance' -Passed (
        $clientAcceptedCatchUp[0].Groups['key'].Value -ne $rejectedKey) -Details "Rejected=$rejectedKey Accepted=$recoveryKey"
    Add-Check -Name 'Recovery uses a new key' -Passed (
        $recoveryKey -ne $rejectedKey -and
        $clientPredicted[1].Groups['key'].Value -eq $recoveryKey -and
        $hostAuthorityCommitted[0].Groups['key'].Value -eq $recoveryKey) -Details "Rejected=$rejectedKey Recovery=$recoveryKey"
    Add-Check -Name 'Ability spec is stable across reject and recovery' -Passed (
        $clientRejected[0].Groups['spec'].Value -eq $spec -and
        $clientPredicted[0].Groups['spec'].Value -eq $spec -and
        $clientPredicted[1].Groups['spec'].Value -eq $spec -and
        $hostAuthorityCommitted[0].Groups['spec'].Value -eq $spec -and
        $clientAcceptedCatchUp[0].Groups['spec'].Value -eq $spec) -Details "Spec=$spec"
    Add-Check -Name 'TrialId is stable across both processes' -Passed (
        $hostArmed[0].Groups['trial'].Value -eq $trial -and
        $hostArmResult[0].Groups['trial'].Value -eq $trial -and
        $clientArmConfirmed[0].Groups['trial'].Value -eq $trial -and
        $clientForcedInput[0].Groups['trial'].Value -eq $trial -and
        $clientRecoveryInput[0].Groups['trial'].Value -eq $trial -and
        $clientPass[0].Groups['trial'].Value -eq $trial) -Details "TrialId=$trial"

    $predictedRejectFields = Convert-LineToFields $clientPredicted[0].Value
    foreach ($expectedField in @{
        TrialId = $trial
        Spec = $spec
        PredictionKey = $rejectedKey
        EnergyBase = '100.0'
        EnergyCurrent = '70.0'
        CostGECount = '1'
        CooldownGECount = '1'
        CooldownTagCount = '1'
        PersistentGECount = '1'
        ImmuneCount = '1'
        PendingGECount = '1'
        PendingCueCount = '1'
    }.GetEnumerator()) {
        Add-FieldCheck -Name "PredictedReject $($expectedField.Key)" -Fields $predictedRejectFields -Field $expectedField.Key -Expected $expectedField.Value
    }

    $predictedRecoveryFields = Convert-LineToFields $clientPredicted[1].Value
    foreach ($expectedField in @{
        TrialId = $trial
        Spec = $spec
        PredictionKey = $recoveryKey
        EnergyBase = '100.0'
        EnergyCurrent = '70.0'
        CostGECount = '1'
        CooldownGECount = '1'
        CooldownTagCount = '1'
        PersistentGECount = '1'
        ImmuneCount = '1'
        PendingGECount = '1'
        PendingCueCount = '1'
    }.GetEnumerator()) {
        Add-FieldCheck -Name "PredictedRecovery $($expectedField.Key)" -Fields $predictedRecoveryFields -Field $expectedField.Key -Expected $expectedField.Value
    }

    if ($clientPostRejectCatchUp.Count -gt 0) {
        $wrongPostRejectKeys = @($clientPostRejectCatchUp | Where-Object {
            $_.Groups['key'].Value -ne $rejectedKey
        })
        Add-Check -Name 'Post-reject CatchUp is classified as bookkeeping only' -Passed (
            $wrongPostRejectKeys.Count -eq 0) -Details "Count=$($clientPostRejectCatchUp.Count) RejectedKey=$rejectedKey"
    }

    if ($postRejectSnapshot.Count -eq 1) {
        $fields = Convert-LineToFields $postRejectSnapshot[0].Value
        foreach ($expectedField in @{
            TrialId = $trial
            RejectedKey = $rejectedKey
            CaughtUpKey = '0'
            EnergyBase = '100.0'
            EnergyCurrent = '100.0'
            CostGECount = '0'
            CooldownGECount = '0'
            ImmunityCooldown = '0'
            PersistentGECount = '0'
            ImmuneCount = '0'
            PendingGECount = '0'
            PendingCue = '0'
            PendingVisual = '0'
        }.GetEnumerator()) {
            Add-FieldCheck -Name "PostReject $($expectedField.Key)" -Fields $fields -Field $expectedField.Key -Expected $expectedField.Value
        }
    }

    if ($postRecoverySnapshot.Count -eq 1) {
        $fields = Convert-LineToFields $postRecoverySnapshot[0].Value
        foreach ($expectedField in @{
            TrialId = $trial
            RejectedKey = $rejectedKey
            CaughtUpKey = $recoveryKey
            EnergyBase = '70.0'
            EnergyCurrent = '70.0'
            CostGECount = '0'
            CooldownGECount = '1'
            ImmunityCooldown = '1'
            PersistentGECount = '1'
            ImmuneCount = '1'
            PendingGECount = '0'
            PendingCue = '0'
            PendingVisual = '0'
        }.GetEnumerator()) {
            Add-FieldCheck -Name "PostRecovery $($expectedField.Key)" -Fields $fields -Field $expectedField.Key -Expected $expectedField.Value
        }
    }

    $clientOrder = @(
        $clientArmConfirmed[0].Index,
        $clientForcedInput[0].Index,
        $clientPredicted[0].Index,
        $clientRejected[0].Index,
        $postRejectSnapshot[0].Index,
        $clientRecoveryInput[0].Index,
        $clientPredicted[1].Index,
        $clientAcceptedCatchUp[0].Index,
        $postRecoverySnapshot[0].Index,
        $clientPass[0].Index
    )
    Add-Check -Name 'Client event order is valid' -Passed (Test-StrictlyIncreasing $clientOrder) -Details ($clientOrder -join ' < ')
    $hostOrder = @(
        $hostArmed[0].Index,
        $hostRejected[0].Index,
        $hostAuthorityCommitted[0].Index
    )
    Add-Check -Name 'Host event order is valid' -Passed (Test-StrictlyIncreasing $hostOrder) -Details ($hostOrder -join ' < ')
}

$cueOnActive = Get-Matches $clientLog 'GAS_CUE_HANDLER[^\r\n]*Cue=GameplayCue\.Coop\.Prediction\.Pending Event=EGameplayCueEvent::OnActive[^\r\n]*'
$cueWhileActive = Get-Matches $clientLog 'GAS_CUE_HANDLER[^\r\n]*Cue=GameplayCue\.Coop\.Prediction\.Pending Event=EGameplayCueEvent::WhileActive[^\r\n]*'
$cueRemoved = Get-Matches $clientLog 'GAS_CUE_HANDLER[^\r\n]*Cue=GameplayCue\.Coop\.Prediction\.Pending Event=EGameplayCueEvent::Removed[^\r\n]*'
$immunityCueOnActive = Get-Matches $clientLog 'GAS_CUE_HANDLER[^\r\n]*Cue=GameplayCue\.Coop\.State\.Immunity Event=EGameplayCueEvent::OnActive[^\r\n]*'
$immunityCueWhileActive = Get-Matches $clientLog 'GAS_CUE_HANDLER[^\r\n]*Cue=GameplayCue\.Coop\.State\.Immunity Event=EGameplayCueEvent::WhileActive[^\r\n]*'
$immunityCueRemoved = Get-Matches $clientLog 'GAS_CUE_HANDLER[^\r\n]*Cue=GameplayCue\.Coop\.State\.Immunity Event=EGameplayCueEvent::Removed[^\r\n]*'
Add-Check -Name 'Pending Cue entered twice' -Passed (
    $cueOnActive.Count -eq 2 -and $cueWhileActive.Count -eq 2) -Details "OnActive=$($cueOnActive.Count) WhileActive=$($cueWhileActive.Count)"
Add-Check -Name 'Rejected Pending Cue emitted one Removed callback' -Passed (
    $cueRemoved.Count -eq 1) -Details "Removed=$($cueRemoved.Count); accepted presentation is explicitly reconciled"
Add-Check -Name 'Immunity Cue entered twice' -Passed (
    $immunityCueOnActive.Count -eq 2 -and $immunityCueWhileActive.Count -eq 2) -Details "OnActive=$($immunityCueOnActive.Count) WhileActive=$($immunityCueWhileActive.Count)"
Add-Check -Name 'Immunity Cue emitted reject cleanup and recovery expiry' -Passed (
    $immunityCueRemoved.Count -eq 2) -Details "Removed=$($immunityCueRemoved.Count); first is reject cleanup, second is recovery natural expiry after checkpoint"
if ($immunityCueRemoved.Count -eq 2 -and $clientRejected.Count -eq 1 -and $postRecoverySnapshot.Count -eq 1) {
    Add-Check -Name 'Immunity Cue Removed callbacks are ordered by transaction' -Passed (
        $immunityCueRemoved[0].Index -lt $clientRejected[0].Index -and
        $immunityCueRemoved[1].Index -gt $postRecoverySnapshot[0].Index) -Details (
        "$($immunityCueRemoved[0].Index) < Reject=$($clientRejected[0].Index); " +
        "$($immunityCueRemoved[1].Index) > RecoveryCheckpoint=$($postRecoverySnapshot[0].Index)")
}
$failedChecks = @($checks | Where-Object { -not $_.Passed })
$passed = $failedChecks.Count -eq 0

$summary = [pscustomobject]@{
    RunId = $RunId
    Passed = $passed
    Network = [pscustomobject]@{
        PktLagPerDirectionMs = $lag
        ApproxRoundTripLagMs = if ($runInfo.ContainsKey('ApproxRoundTripLagMs')) { $runInfo['ApproxRoundTripLagMs'] } else { '<missing>' }
        PktLossPercentPerDirection = $loss
    }
    PredictionPendingCueInventory = [pscustomobject]@{
        OnActive = $cueOnActive.Count
        WhileActive = $cueWhileActive.Count
        Removed = $cueRemoved.Count
    }
    ImmunityCueInventory = [pscustomobject]@{
        OnActive = $immunityCueOnActive.Count
        WhileActive = $immunityCueWhileActive.Count
        Removed = $immunityCueRemoved.Count
    }
    Checks = $checks
}

$jsonPath = Join-Path $runDirectory 'M6Summary.json'
$markdownPath = Join-Path $runDirectory 'M6Summary.md'
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $jsonPath -Encoding utf8

$markdown = @(
    "# GAS M6 verification: $RunId"
    ''
    "- Result: **$(if ($passed) { 'PASS' } else { 'FAIL' })**"
    "- Per-direction lag: $lag ms"
    "- Approximate RTT: $($summary.Network.ApproxRoundTripLagMs) ms"
    "- Per-direction loss: $loss%"
    "- Pending Cue inventory: OnActive=$($cueOnActive.Count), WhileActive=$($cueWhileActive.Count), Removed=$($cueRemoved.Count)"
    "- Immunity Cue inventory: OnActive=$($immunityCueOnActive.Count), WhileActive=$($immunityCueWhileActive.Count), Removed=$($immunityCueRemoved.Count)"
    ''
    '| Check | Result | Details |'
    '|---|---|---|'
)
foreach ($check in $checks) {
    $escapedDetails = $check.Details.Replace('|', '\|')
    $markdown += "| $($check.Name) | $(if ($check.Passed) { 'PASS' } else { 'FAIL' }) | $escapedDetails |"
}
$markdown | Set-Content -LiteralPath $markdownPath -Encoding utf8

Write-Output "RunId=$RunId Result=$(if ($passed) { 'PASS' } else { 'FAIL' }) Checks=$($checks.Count) Failed=$($failedChecks.Count)"
Write-Output "SummaryJson=$jsonPath"
Write-Output "SummaryMarkdown=$markdownPath"
foreach ($failure in $failedChecks) {
    Write-Output "FAILED: $($failure.Name) -- $($failure.Details)"
}

if (-not $passed) {
    exit 1
}
exit 0
