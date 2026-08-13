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

function Read-KeyValueFile {
    param([string]$Path)

    $result = @{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $result
    }

    foreach ($line in Get-Content -LiteralPath $Path) {
        $match = [regex]::Match($line, '^(?<key>[^=]+)=(?<value>.*)$')
        if ($match.Success) {
            $result[$match.Groups['key'].Value] = $match.Groups['value'].Value
        }
    }
    return $result
}

function Get-MapValue {
    param(
        [hashtable]$Map,
        [string]$Key,
        [string]$Default = '<missing>'
    )

    if ($Map.ContainsKey($Key)) {
        return [string]$Map[$Key]
    }
    return $Default
}

if (-not (Test-Path -LiteralPath $BaselineRoot -PathType Container)) {
    Write-Error "GAS baseline root does not exist: $BaselineRoot"
    exit 1
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    foreach ($candidate in Get-ChildItem -LiteralPath $BaselineRoot -Directory |
        Sort-Object LastWriteTime -Descending) {
        $candidateInfoPath = Join-Path $candidate.FullName 'RunInfo.txt'
        $candidateInfo = Read-KeyValueFile -Path $candidateInfoPath
        if ((Get-MapValue -Map $candidateInfo -Key 'Stage') -eq 'M6Intent') {
            $RunId = $candidate.Name
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    Write-Error "No Stage=M6Intent run was found beneath: $BaselineRoot"
    exit 1
}

$runDirectory = Join-Path $BaselineRoot $RunId
if (-not (Test-Path -LiteralPath $runDirectory -PathType Container)) {
    Write-Error "M6Intent run directory does not exist: $runDirectory"
    exit 1
}

$runInfoPath = Join-Path $runDirectory 'RunInfo.txt'
$hostLogPath = Join-Path $runDirectory 'Host.log'
$clientLogPath = Join-Path $runDirectory 'Client.log'
$runInfoExists = Test-Path -LiteralPath $runInfoPath -PathType Leaf
$hostLogExists = Test-Path -LiteralPath $hostLogPath -PathType Leaf
$clientLogExists = Test-Path -LiteralPath $clientLogPath -PathType Leaf
$runInfo = Read-KeyValueFile -Path $runInfoPath
$hostLog = if ($hostLogExists) { Get-Content -LiteralPath $hostLogPath -Raw } else { '' }
$clientLog = if ($clientLogExists) { Get-Content -LiteralPath $clientLogPath -Raw } else { '' }
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

function Get-LogMatches {
    param(
        [string]$Text,
        [string]$Pattern
    )

    return ,@([regex]::Matches(
        $Text,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Multiline))
}

function Get-ShotMatches {
    param(
        [System.Text.RegularExpressions.Match[]]$Matches,
        [int]$ShotId
    )

    return ,@($Matches | Where-Object {
        $_.Groups['shot'].Value -eq [string]$ShotId
    })
}

function Format-ShotResultInventory {
    param([System.Text.RegularExpressions.Match[]]$Matches)

    $items = @($Matches | ForEach-Object {
        "$($_.Groups['shot'].Value):$($_.Groups['result'].Value)"
    })
    if ($items.Count -eq 0) {
        return '<none>'
    }
    return $items -join ', '
}

function Format-ShotReasonInventory {
    param([System.Text.RegularExpressions.Match[]]$Matches)

    $items = @($Matches | ForEach-Object {
        "$($_.Groups['shot'].Value):$($_.Groups['reason'].Value)"
    })
    if ($items.Count -eq 0) {
        return '<none>'
    }
    return $items -join ', '
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

Add-Check -Name 'Evidence file RunInfo.txt exists' -Passed $runInfoExists -Details $runInfoPath
Add-Check -Name 'Evidence file Host.log exists' -Passed $hostLogExists -Details $hostLogPath
Add-Check -Name 'Evidence file Client.log exists' -Passed $clientLogExists -Details $clientLogPath

$stage = Get-MapValue -Map $runInfo -Key 'Stage'
$autoSequence = Get-MapValue -Map $runInfo -Key 'ClientAutoSequence'
$hostReady = Get-MapValue -Map $runInfo -Key 'HostReady'
$clientJoined = Get-MapValue -Map $runInfo -Key 'ClientJoined'
Add-Check -Name 'RunInfo Stage is M6Intent' -Passed ($stage -eq 'M6Intent') -Details "Stage=$stage"
Add-Check -Name 'RunInfo client auto sequence is enabled' -Passed ($autoSequence -eq 'True') -Details "ClientAutoSequence=$autoSequence"
Add-Check -Name 'RunInfo host reached ready state' -Passed ($hostReady -eq 'true') -Details "HostReady=$hostReady"
Add-Check -Name 'RunInfo client joined host' -Passed ($clientJoined -eq 'true') -Details "ClientJoined=$clientJoined"

$severePattern = '(?i)Fatal error:|Critical error:|Ensure condition failed|Assertion failed'
$hostSevere = Get-LogMatches -Text $hostLog -Pattern $severePattern
$clientSevere = Get-LogMatches -Text $clientLog -Pattern $severePattern
Add-Check -Name 'Host has no fatal, critical, ensure, or assertion' -Passed (
    $hostLogExists -and $hostSevere.Count -eq 0) -Details "Matches=$($hostSevere.Count)"
Add-Check -Name 'Client has no fatal, critical, ensure, or assertion' -Passed (
    $clientLogExists -and $clientSevere.Count -eq 0) -Details "Matches=$($clientSevere.Count)"

$clientSequencePass = Get-LogMatches -Text $clientLog -Pattern (
    'GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Pass(?:\s|$)')
$hostSequencePass = Get-LogMatches -Text $hostLog -Pattern (
    'GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Pass(?:\s|$)')
$sequenceFail = Get-LogMatches -Text "$hostLog`n$clientLog" -Pattern (
    'GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Fail[^\r\n]*')
Add-Check -Name 'M6Intent sequence completed with one client Pass' -Passed (
    $clientSequencePass.Count -eq 1 -and $hostSequencePass.Count -eq 0) -Details (
    "ClientPass=$($clientSequencePass.Count) HostPass=$($hostSequencePass.Count)")
Add-Check -Name 'M6Intent sequence emitted no Fail' -Passed (
    $sequenceFail.Count -eq 0) -Details "Fail=$($sequenceFail.Count)"

$clientResults = Get-LogMatches -Text $clientLog -Pattern (
    'GAS_M6_INTENT Phase=ClientResult ShotId=(?<shot>[0-9]+) Result=(?<result>[A-Za-z0-9_]+) Owner=(?<owner>[^\s]+)')
$expectedResults = @(
    [pscustomobject]@{ ShotId = 1; Result = 'Accepted'; Label = 'Shot1 Accepted' },
    [pscustomobject]@{ ShotId = 1; Result = 'Duplicate'; Label = 'Shot1 Duplicate' },
    [pscustomobject]@{ ShotId = 2; Result = 'InvalidOrigin'; Label = 'Shot2 InvalidOrigin' },
    [pscustomobject]@{ ShotId = 3; Result = 'InvalidDirection'; Label = 'Shot3 InvalidDirection' },
    [pscustomobject]@{ ShotId = 4; Result = 'InvalidTime'; Label = 'Shot4 InvalidTime' },
    [pscustomobject]@{ ShotId = 5; Result = 'InvalidTime'; Label = 'Shot5 InvalidTime' },
    [pscustomobject]@{ ShotId = 6; Result = 'Miss'; Label = 'Shot6 Miss' },
    [pscustomobject]@{ ShotId = 7; Result = 'Accepted'; Label = 'Shot7 Accepted' }
)
$clientResultInventoryText = Format-ShotResultInventory -Matches @($clientResults)
Add-Check -Name 'Client emitted exactly eight intent results' -Passed (
    $clientResults.Count -eq $expectedResults.Count) -Details (
    "Expected=$($expectedResults.Count) Actual=$($clientResults.Count) Events=$clientResultInventoryText")

$orderedClientResultMatches = @()
$allExpectedClientResultsAreUnique = $true
foreach ($expected in $expectedResults) {
    $matching = @($clientResults | Where-Object {
        $_.Groups['shot'].Value -eq [string]$expected.ShotId -and
        $_.Groups['result'].Value -eq $expected.Result
    })
    Add-Check -Name "Client result $($expected.Label) exactly once" -Passed (
        $matching.Count -eq 1) -Details (
        "Expected=1 Actual=$($matching.Count) Events=$clientResultInventoryText")
    if ($matching.Count -eq 1) {
        $orderedClientResultMatches += $matching[0]
    }
    else {
        $allExpectedClientResultsAreUnique = $false
    }
}
$clientResultIndexes = @($orderedClientResultMatches | ForEach-Object { $_.Index })
Add-Check -Name 'Client intent results are in the expected order' -Passed (
    $allExpectedClientResultsAreUnique -and
    (Test-StrictlyIncreasing -Values $clientResultIndexes)) -Details (
    "Events=$clientResultInventoryText")

$hostCommits = Get-LogMatches -Text $hostLog -Pattern (
    'GAS_M6_INTENT Phase=Committed ShotId=(?<shot>[0-9]+) Spec=(?<spec>[^\s]+) PredictionKey=(?<key>[^\s]+) Target=(?<target>[^\s]+) RemainingHealth=(?<health>-?[0-9]+(?:\.[0-9]+)?)')
$commitInventoryText = @($hostCommits | ForEach-Object {
    "$($_.Groups['shot'].Value):$($_.Groups['target'].Value)"
}) -join ', '
if ([string]::IsNullOrWhiteSpace($commitInventoryText)) {
    $commitInventoryText = '<none>'
}
Add-Check -Name 'Host emitted exactly two commits' -Passed (
    $hostCommits.Count -eq 2) -Details "Expected=2 Actual=$($hostCommits.Count) Events=$commitInventoryText"

$orderedCommitMatches = @()
foreach ($shotId in @(1, 7)) {
    $matching = Get-ShotMatches -Matches @($hostCommits) -ShotId $shotId
    $matchingDummy = @($matching | Where-Object {
        $_.Groups['target'].Value -match '^multiplayerGASTargetDummy_[0-9]+$'
    })
    Add-Check -Name "Host committed Shot$shotId to dummy exactly once" -Passed (
        $matching.Count -eq 1 -and $matchingDummy.Count -eq 1) -Details (
        "ShotMatches=$($matching.Count) DummyMatches=$($matchingDummy.Count) Events=$commitInventoryText")
    if ($matching.Count -eq 1) {
        $orderedCommitMatches += $matching[0]
    }
}
$unexpectedCommits = @($hostCommits | Where-Object {
    [int]$_.Groups['shot'].Value -notin @(1, 7)
})
Add-Check -Name 'Host committed no shot except Shot1 and Shot7' -Passed (
    $unexpectedCommits.Count -eq 0) -Details "Unexpected=$($unexpectedCommits.Count) Events=$commitInventoryText"
$commitIndexes = @($orderedCommitMatches | ForEach-Object { $_.Index })
Add-Check -Name 'Host commits are ordered Shot1 then Shot7' -Passed (
    $orderedCommitMatches.Count -eq 2 -and
    (Test-StrictlyIncreasing -Values $commitIndexes)) -Details "Events=$commitInventoryText"

$hostRejections = Get-LogMatches -Text $hostLog -Pattern (
    'GAS_M6_INTENT Phase=AuthorityRejected ShotId=(?<shot>[0-9]+) Reason=(?<reason>[A-Za-z0-9_]+) Spec=(?<spec>[^\s]+) PredictionKey=(?<key>[^\s]+) Avatar=(?<avatar>[^\s]+)')
$expectedRejections = @(
    [pscustomobject]@{ ShotId = 1; Reason = 'Duplicate'; Label = 'Shot1 Duplicate' },
    [pscustomobject]@{ ShotId = 2; Reason = 'InvalidOrigin'; Label = 'Shot2 InvalidOrigin' },
    [pscustomobject]@{ ShotId = 3; Reason = 'InvalidDirection'; Label = 'Shot3 InvalidDirection' },
    [pscustomobject]@{ ShotId = 4; Reason = 'InvalidTime'; Label = 'Shot4 InvalidTime' },
    [pscustomobject]@{ ShotId = 5; Reason = 'InvalidTime'; Label = 'Shot5 InvalidTime' },
    [pscustomobject]@{ ShotId = 6; Reason = 'Miss'; Label = 'Shot6 Miss' }
)
$hostRejectionInventoryText = Format-ShotReasonInventory -Matches @($hostRejections)
Add-Check -Name 'Host emitted exactly six semantic rejections' -Passed (
    $hostRejections.Count -eq $expectedRejections.Count) -Details (
    "Expected=$($expectedRejections.Count) Actual=$($hostRejections.Count) Events=$hostRejectionInventoryText")

$orderedHostRejectionMatches = @()
$allExpectedHostRejectionsAreUnique = $true
foreach ($expected in $expectedRejections) {
    $matching = @($hostRejections | Where-Object {
        $_.Groups['shot'].Value -eq [string]$expected.ShotId -and
        $_.Groups['reason'].Value -eq $expected.Reason
    })
    Add-Check -Name "Host rejection $($expected.Label) exactly once" -Passed (
        $matching.Count -eq 1) -Details (
        "Expected=1 Actual=$($matching.Count) Events=$hostRejectionInventoryText")
    if ($matching.Count -eq 1) {
        $orderedHostRejectionMatches += $matching[0]
    }
    else {
        $allExpectedHostRejectionsAreUnique = $false
    }
}
$hostRejectionIndexes = @($orderedHostRejectionMatches | ForEach-Object { $_.Index })
Add-Check -Name 'Host semantic rejections are in the expected order' -Passed (
    $allExpectedHostRejectionsAreUnique -and
    (Test-StrictlyIncreasing -Values $hostRejectionIndexes)) -Details (
    "Events=$hostRejectionInventoryText")

$hostDecisionMatches = @($hostCommits) + @($hostRejections)
$hostDecisionSpecs = @($hostDecisionMatches | ForEach-Object {
    $_.Groups['spec'].Value
} | Sort-Object -Unique)
$stableSpec = if ($hostDecisionSpecs.Count -eq 1) {
    [string]$hostDecisionSpecs[0]
} else {
    '<missing-or-ambiguous>'
}
Add-Check -Name 'Host decision ability Spec is present and stable' -Passed (
    $hostDecisionMatches.Count -eq 8 -and
    $hostDecisionSpecs.Count -eq 1 -and
    -not [string]::IsNullOrWhiteSpace($stableSpec) -and
    $stableSpec -ne '<missing-or-ambiguous>') -Details (
    "Decisions=$($hostDecisionMatches.Count) Specs=$($hostDecisionSpecs -join ',')")

$checkpoints = Get-LogMatches -Text $clientLog -Pattern (
    'GAS_M6_INTENT_AUTO Phase=Checkpoint Name=(?<name>[^\s]+) ShotId=(?<shot>[0-9]+) Result=(?<result>[A-Za-z0-9_]+) Energy=(?<energy>-?[0-9]+(?:\.[0-9]+)?) Cooldown=(?<cooldown>-?[0-9]+)')
$expectedCheckpoints = @(
    [pscustomobject]@{ Name = 'ValidAccepted'; ShotId = 1; Result = 'Accepted'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'DuplicateRejected'; ShotId = 1; Result = 'Duplicate'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'OriginRejected'; ShotId = 2; Result = 'InvalidOrigin'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'DirectionRejected'; ShotId = 3; Result = 'InvalidDirection'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'StaleTimeRejected'; ShotId = 4; Result = 'InvalidTime'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'FutureTimeRejected'; ShotId = 5; Result = 'InvalidTime'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'MissRejected'; ShotId = 6; Result = 'Miss'; Energy = '90.0'; Cooldown = '0' },
    [pscustomobject]@{ Name = 'RecoveryAccepted'; ShotId = 7; Result = 'Accepted'; Energy = '80.0'; Cooldown = '1' }
)
$checkpointInventoryText = @($checkpoints | ForEach-Object {
    "$($_.Groups['name'].Value):Shot$($_.Groups['shot'].Value):$($_.Groups['result'].Value):E$($_.Groups['energy'].Value):CD$($_.Groups['cooldown'].Value)"
}) -join ', '
if ([string]::IsNullOrWhiteSpace($checkpointInventoryText)) {
    $checkpointInventoryText = '<none>'
}
Add-Check -Name 'Client emitted exactly eight state checkpoints' -Passed (
    $checkpoints.Count -eq $expectedCheckpoints.Count) -Details (
    "Expected=$($expectedCheckpoints.Count) Actual=$($checkpoints.Count)")

$orderedCheckpointMatches = @()
$allExpectedCheckpointsAreUnique = $true
foreach ($expected in $expectedCheckpoints) {
    $matching = @($checkpoints | Where-Object {
        $_.Groups['name'].Value -eq $expected.Name -and
        $_.Groups['shot'].Value -eq [string]$expected.ShotId -and
        $_.Groups['result'].Value -eq $expected.Result -and
        $_.Groups['energy'].Value -eq $expected.Energy -and
        $_.Groups['cooldown'].Value -eq $expected.Cooldown
    })
    Add-Check -Name "Checkpoint $($expected.Name) has exact result and state" -Passed (
        $matching.Count -eq 1) -Details (
        "Expected=Shot$($expected.ShotId)/$($expected.Result)/Energy$($expected.Energy)/Cooldown$($expected.Cooldown) ActualMatches=$($matching.Count)")
    if ($matching.Count -eq 1) {
        $orderedCheckpointMatches += $matching[0]
    }
    else {
        $allExpectedCheckpointsAreUnique = $false
    }
}
$checkpointIndexes = @($orderedCheckpointMatches | ForEach-Object { $_.Index })
Add-Check -Name 'Client checkpoints are in the expected order' -Passed (
    $allExpectedCheckpointsAreUnique -and
    (Test-StrictlyIncreasing -Values $checkpointIndexes)) -Details (
    "Events=$checkpointInventoryText")

$resultsPrecedeCheckpoints = $allExpectedClientResultsAreUnique -and
    $allExpectedCheckpointsAreUnique -and
    $orderedClientResultMatches.Count -eq $orderedCheckpointMatches.Count
if ($resultsPrecedeCheckpoints) {
    for ($index = 0; $index -lt $orderedClientResultMatches.Count; ++$index) {
        if ($orderedClientResultMatches[$index].Index -ge $orderedCheckpointMatches[$index].Index) {
            $resultsPrecedeCheckpoints = $false
            break
        }
    }
}
Add-Check -Name 'Each client result precedes its state checkpoint' -Passed (
    $resultsPrecedeCheckpoints) -Details "Pairs=$($orderedCheckpointMatches.Count)"

$hostTraces = Get-LogMatches -Text $hostLog -Pattern (
    'GAS_M6_INTENT Phase=AuthorityTrace ShotId=(?<shot>[0-9]+)[^\r\n]* Target=(?<target>[^\s]+) Impact=')
$damageContexts = Get-LogMatches -Text $hostLog -Pattern (
    'GAS_DAMAGE_CONTEXT Target=(?<target>[^\s]+) Damage=(?<damage>-?[0-9]+(?:\.[0-9]+)?) Critical=(?<critical>[^\s]+) HitType=(?<hitType>[^\s]+)')
$damageExecutions = Get-LogMatches -Text $hostLog -Pattern (
    'GAS_DAMAGE_EXEC Base=(?<base>-?[0-9]+(?:\.[0-9]+)?) Health=(?<health>-?[0-9]+(?:\.[0-9]+)?)/(?<maxHealth>-?[0-9]+(?:\.[0-9]+)?) Vulnerability=(?<vulnerability>-?[0-9]+) Critical=(?<critical>[^\s]+) Final=(?<final>-?[0-9]+(?:\.[0-9]+)?)')
$dummyDamageContexts = @($damageContexts | Where-Object {
    $_.Groups['target'].Value -match '^multiplayerGASTargetDummy_[0-9]+$'
})
Add-Check -Name 'Host traced exactly the two accepted shots' -Passed (
    $hostTraces.Count -eq 2 -and
    (Get-ShotMatches -Matches @($hostTraces) -ShotId 1).Count -eq 1 -and
    (Get-ShotMatches -Matches @($hostTraces) -ShotId 7).Count -eq 1) -Details (
    "TraceCount=$($hostTraces.Count) Shots=$(@($hostTraces | ForEach-Object { $_.Groups['shot'].Value }) -join ',')")
Add-Check -Name 'Host executed exactly two damage contexts and both target dummy' -Passed (
    $damageContexts.Count -eq 2 -and $dummyDamageContexts.Count -eq 2) -Details (
    "AllDamageContexts=$($damageContexts.Count) DummyDamageContexts=$($dummyDamageContexts.Count)")
Add-Check -Name 'Host executed exactly two damage calculations' -Passed (
    $damageExecutions.Count -eq 2) -Details "GAS_DAMAGE_EXEC=$($damageExecutions.Count)"

$allDamageAmountsPositive = $damageContexts.Count -eq 2
foreach ($damageContext in $damageContexts) {
    $damageAmount = [double]::Parse(
        $damageContext.Groups['damage'].Value,
        [System.Globalization.CultureInfo]::InvariantCulture)
    if ($damageAmount -le 0.0) {
        $allDamageAmountsPositive = $false
    }
}
Add-Check -Name 'Both dummy damage executions have positive damage' -Passed (
    $allDamageAmountsPositive) -Details (
    "Damage=$(@($damageContexts | ForEach-Object { $_.Groups['damage'].Value }) -join ',')")

foreach ($shotId in @(1, 7)) {
    $trace = Get-ShotMatches -Matches @($hostTraces) -ShotId $shotId
    $commit = Get-ShotMatches -Matches @($hostCommits) -ShotId $shotId
    $damageInTransaction = @()
    $executionInTransaction = @()
    $transactionConsistent = $false
    if ($trace.Count -eq 1 -and $commit.Count -eq 1) {
        $damageInTransaction = @($dummyDamageContexts | Where-Object {
            $_.Index -gt $trace[0].Index -and $_.Index -lt $commit[0].Index
        })
        $executionInTransaction = @($damageExecutions | Where-Object {
            $_.Index -gt $trace[0].Index -and $_.Index -lt $commit[0].Index
        })
    }
    if ($damageInTransaction.Count -eq 1 -and $executionInTransaction.Count -eq 1) {
        $healthBefore = [double]::Parse(
            $executionInTransaction[0].Groups['health'].Value,
            [System.Globalization.CultureInfo]::InvariantCulture)
        $finalDamage = [double]::Parse(
            $executionInTransaction[0].Groups['final'].Value,
            [System.Globalization.CultureInfo]::InvariantCulture)
        $contextDamage = [double]::Parse(
            $damageInTransaction[0].Groups['damage'].Value,
            [System.Globalization.CultureInfo]::InvariantCulture)
        $remainingHealth = [double]::Parse(
            $commit[0].Groups['health'].Value,
            [System.Globalization.CultureInfo]::InvariantCulture)
        $transactionConsistent =
            $trace[0].Groups['target'].Value -eq $damageInTransaction[0].Groups['target'].Value -and
            $damageInTransaction[0].Groups['target'].Value -eq $commit[0].Groups['target'].Value -and
            [math]::Abs($finalDamage - $contextDamage) -lt 0.01 -and
            [math]::Abs(($healthBefore - $finalDamage) - $remainingHealth) -lt 0.11
    }
    Add-Check -Name "Accepted Shot$shotId executed damage on dummy exactly once" -Passed (
        $trace.Count -eq 1 -and
        $commit.Count -eq 1 -and
        $trace[0].Index -lt $commit[0].Index -and
        $executionInTransaction.Count -eq 1 -and
        $damageInTransaction.Count -eq 1 -and
        $transactionConsistent) -Details (
        "Trace=$($trace.Count) Commit=$($commit.Count) DamageExecBetween=$($executionInTransaction.Count) DummyDamageContextBetween=$($damageInTransaction.Count) TargetAndHealthConsistent=$transactionConsistent")
}

$failedChecks = @($checks | Where-Object { -not $_.Passed })
$passed = $failedChecks.Count -eq 0
$clientResultInventory = @($clientResults | ForEach-Object {
    [pscustomobject]@{
        ShotId = [int]$_.Groups['shot'].Value
        Result = $_.Groups['result'].Value
        Owner = $_.Groups['owner'].Value
    }
})
$hostCommitInventory = @($hostCommits | ForEach-Object {
    [pscustomobject]@{
        ShotId = [int]$_.Groups['shot'].Value
        Target = $_.Groups['target'].Value
        RemainingHealth = $_.Groups['health'].Value
        Spec = $_.Groups['spec'].Value
        PredictionKey = $_.Groups['key'].Value
    }
})
$hostRejectionInventory = @($hostRejections | ForEach-Object {
    [pscustomobject]@{
        ShotId = [int]$_.Groups['shot'].Value
        Reason = $_.Groups['reason'].Value
        Spec = $_.Groups['spec'].Value
        PredictionKey = $_.Groups['key'].Value
    }
})
$checkpointInventory = @($checkpoints | ForEach-Object {
    [pscustomobject]@{
        Name = $_.Groups['name'].Value
        ShotId = [int]$_.Groups['shot'].Value
        Result = $_.Groups['result'].Value
        Energy = $_.Groups['energy'].Value
        Cooldown = [int]$_.Groups['cooldown'].Value
    }
})
$damageInventory = @($damageContexts | ForEach-Object {
    [pscustomobject]@{
        Target = $_.Groups['target'].Value
        Damage = $_.Groups['damage'].Value
        Critical = $_.Groups['critical'].Value
        HitType = $_.Groups['hitType'].Value
    }
})
$damageExecutionInventory = @($damageExecutions | ForEach-Object {
    [pscustomobject]@{
        Base = $_.Groups['base'].Value
        HealthBefore = $_.Groups['health'].Value
        MaxHealth = $_.Groups['maxHealth'].Value
        Vulnerability = [int]$_.Groups['vulnerability'].Value
        Critical = $_.Groups['critical'].Value
        FinalDamage = $_.Groups['final'].Value
    }
})
$evidenceLimitations = @(
    'GAS_DAMAGE_EXEC and GAS_DAMAGE_CONTEXT do not carry ShotId; attribution to Shot1 and Shot7 is verified by exact total counts and AuthorityTrace -> GAS_DAMAGE_EXEC -> GAS_DAMAGE_CONTEXT -> Committed ordering for each accepted transaction.'
)
$summary = [pscustomobject]@{
    RunId = $RunId
    Stage = $stage
    Passed = $passed
    CheckCount = $checks.Count
    PassedCheckCount = $checks.Count - $failedChecks.Count
    FailedCheckCount = $failedChecks.Count
    EvidenceFiles = [pscustomobject]@{
        RunInfo = $runInfoPath
        HostLog = $hostLogPath
        ClientLog = $clientLogPath
    }
    Evidence = [pscustomobject]@{
        ClientResults = $clientResultInventory
        HostCommits = $hostCommitInventory
        HostRejections = $hostRejectionInventory
        ClientCheckpoints = $checkpointInventory
        HostDamageExecutions = $damageExecutionInventory
        HostDamageContexts = $damageInventory
    }
    EvidenceLimitations = $evidenceLimitations
    Checks = $checks
}

$jsonPath = Join-Path $runDirectory 'M6IntentSummary.json'
$markdownPath = Join-Path $runDirectory 'M6IntentSummary.md'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding utf8

$markdown = @(
    "# GAS M6 Intent verification: $RunId"
    ''
    "- Result: **$(if ($passed) { 'PASS' } else { 'FAIL' })**"
    "- Checks: $($checks.Count) total, $($checks.Count - $failedChecks.Count) passed, $($failedChecks.Count) failed"
    "- Client results: $clientResultInventoryText"
    "- Host commits: $commitInventoryText"
    "- Host rejections: $hostRejectionInventoryText"
    "- Host dummy damage contexts: $($dummyDamageContexts.Count)"
    ''
    '## Evidence limitation'
    ''
    "- $($evidenceLimitations[0])"
    ''
    '## Checks'
    ''
    '| Check | Result | Details |'
    '|---|---|---|'
)
foreach ($check in $checks) {
    $escapedName = ([string]$check.Name).Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
    $escapedDetails = ([string]$check.Details).Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
    $markdown += "| $escapedName | $(if ($check.Passed) { 'PASS' } else { 'FAIL' }) | $escapedDetails |"
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
