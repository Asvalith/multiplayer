[CmdletBinding()]
param(
    [ValidateSet('Regression', 'Scale', 'All')][string]$Suite = 'Regression',
    [ValidateSet('All', 'DedicatedSmoke', 'Flow', 'Keys', 'LateJoin', 'Reconnect', 'ConnectionRetry', 'Ride', 'RideMotion')][string]$Scenario = 'All',
    [ValidateSet('Normal', 'Moderate', 'Harsh')][string[]]$Profiles = @('Normal', 'Moderate', 'Harsh'),
    [ValidateRange(1, 100)][int]$Repeat = 3,
    [ValidateSet('Static', 'Moving', 'All')][string]$Matrix = 'All',
    [ValidateRange(0, 500)][int[]]$StaticCounts = @(0, 50, 200, 500),
    [ValidateRange(0, 20)][int[]]$MovingCounts = @(1, 5, 20),
    [ValidateRange(1, 300)][int]$WarmupSeconds = 5,
    [ValidateRange(5, 600)][int]$SampleSeconds = 30,
    [ValidateRange(30, 1800)][int]$TimeoutSeconds = 180,
    [ValidateRange(1, 60)][int]$PlatformNetHz = 30,
    [ValidateSet('Baseline', 'OrderedVelocity', 'PlatformInertia')][string]$PlatformSyncMode = 'PlatformInertia',
    [ValidateSet('Moving', 'Static')][string]$PlatformMotion = 'Moving',
    [string]$EditorPath = $(if ($env:UE_EDITOR) { $env:UE_EDITOR } else { 'C:/Program Files/Epic Games/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe' }),
    [string]$GameExecutablePath = '',
    [string]$ServerExecutablePath = '',
    [string]$Map = '/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo',
    [ValidateRange(0, 65535)][int]$ListenPort = 0,
    [string]$OutputDirectory = '',
    [switch]$Visible,
    [switch]$Offscreen,
    [switch]$PlanOnly,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Visible -and $Offscreen) { throw 'Choose Visible or Offscreen, not both.' }
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectPath = Join-Path $ProjectRoot 'multiplayer.uproject'
$Utf8 = New-Object System.Text.UTF8Encoding($false)

function Get-PropertyValue {
    param($Object, [string]$Name, $Default = $null)
    if ($null -ne $Object -and $null -ne $Object.PSObject.Properties[$Name]) { return $Object.$Name }
    return $Default
}

function Read-SharedText {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    $stream = [IO.File]::Open($Path, 'Open', 'Read', [IO.FileShare]::ReadWrite)
    $reader = New-Object IO.StreamReader($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose(); $stream.Dispose() }
}

function ConvertFrom-CoopLog {
    param(
        [AllowEmptyString()][string]$Text,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Token,
        [Parameter(Mandatory)][ValidateSet('Server', 'Client', 'Partner')][string]$Role,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Mode
    )
    foreach ($line in ($Text -split '\r?\n')) {
        if ($line -match 'COOP_TEST\s+(\{.*\})\s*$') {
            try { $event = $Matches[1] | ConvertFrom-Json }
            catch { continue } # 并发写入中的最后一行可能不完整，下一次读取再解析。
            # 日志文件可能来自复用的输出目录；旧进程的 READY/DONE 不能替本次测试过关。
            # 同时核对角色与模式，防止服务器日志中的异常事件冒充客户端回执。
            if ($null -ne $event.PSObject.Properties['status'] -and
                (Get-PropertyValue $event 'token' '') -ceq $Token -and
                (Get-PropertyValue $event 'role' '') -ceq $Role -and
                (Get-PropertyValue $event 'mode' '') -ceq $Mode) { Write-Output $event }
        }
    }
}

function Get-CoopEvents {
    param(
        [string]$Path,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Token,
        [Parameter(Mandatory)][ValidateSet('Server', 'Client', 'Partner')][string]$Role,
        [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Mode
    )
    return @(ConvertFrom-CoopLog -Text (Read-SharedText -Path $Path) -Token $Token -Role $Role -Mode $Mode)
}

function Test-RoleDone {
    param([object[]]$Events, [string]$Role)
    $done = @($Events | Where-Object { $_.status -eq 'DONE' -and (Get-PropertyValue $_ 'role' '') -eq $Role })
    if ($done.Count -eq 0) { return $false }
    return ((Get-PropertyValue $done[-1] 'passed' $false) -ceq $true)
}

function Quote-Argument {
    param([string]$Value)
    # 参数只有文件路径、名称和数值，不拼接交给 shell 执行的命令。
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Get-FreeUdpPort {
    $socket = New-Object Net.Sockets.UdpClient(0)
    try { return ([Net.IPEndPoint]$socket.Client.LocalEndPoint).Port } finally { $socket.Dispose() }
}

function Get-StringHash {
    param([string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Test-SourceManifestPath {
    param([string]$RelativePath)
    # 只排除完整目录段，避免把含 Saved 等字样的正常源码文件误删出清单。
    return $RelativePath -notmatch '(^|[\\/])(\.vs|\.git|Binaries|Intermediate|Saved)([\\/]|$)|(^|[\\/])Tests[\\/]Evidence([\\/]|$)'
}

function Get-SourceManifest {
    $files = @()
    foreach ($directory in @('Source', 'Config', 'Scripts', 'Tests')) {
        $directoryPath = Join-Path $ProjectRoot $directory
        if (Test-Path -LiteralPath $directoryPath) {
            $files += @(Get-ChildItem -LiteralPath $directoryPath -File -Recurse | Where-Object {
                Test-SourceManifestPath $_.FullName.Substring($ProjectRoot.Length).TrimStart('\', '/')
            })
        }
    }
    $files += Get-Item -LiteralPath $ProjectPath
    $configPath = Join-Path $ProjectRoot 'Content/Config/Gameplay.json'
    if (Test-Path -LiteralPath $configPath) { $files += Get-Item -LiteralPath $configPath }
    return @($files | Sort-Object FullName -Unique | ForEach-Object {
        [pscustomobject]@{ path = $_.FullName.Substring($ProjectRoot.Length).TrimStart('\', '/').Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
}

function Get-RequiredAssertions {
    param([string]$Mode)
    switch ($Mode) {
        'DedicatedSmoke' { return @('TwoRemotePlayers', 'TwoClientReceipts', 'DedicatedClientState') }
        'Flow' { return @('Join', 'ClientInputReady', 'OverlapKeys', 'PlateDistinctPlayers', 'GateRules', 'GateOpen', 'GateClosed', 'OccupancyUnpossess', 'OccupancyRepossess', 'OccupancyUncontrolledEntry', 'OccupancyLatePossess', 'DestroyedPawnCleanup', 'PlatformEndpoint', 'ClientRide', 'ClientVictoryState', 'ClientRestartFailureRecovery', 'RestartFailureRecovery', 'Restart', 'ClientRestart', 'ClientLeave', 'ServerSurvivesClientLeave', 'PartnerVictory', 'PartnerRestart') }
        'Keys' { return @('Join', 'KeyHeld', 'ClientKeyHeld', 'KeyInstallFailureRollback', 'KeyCommitReentryBlocked', 'KeyInstalledOnce', 'ClientKeyInstalled', 'KeyConsumeCommitted', 'KeyInstallRejectedRetainsKey', 'KeyConsumeRejectedRetainsKey', 'KeyDestroyClearsCarry', 'KeyDropRepick', 'ClientKeyRepicked') }
        'LateJoin' { return @('LateJoinState', 'LateJoinKeyAttachments') }
        'Reconnect' { return @('OutageApplied', 'ConnectionLostDetected', 'ServerConnectionRetained', 'ReconnectAfterOutage', 'ReconnectStateRestored') }
        'ConnectionRetry' { return @('ConnectionFailureRecovered', 'ConnectionRetrySucceeded') }
        'Ride' { return @('ClientRide') }
        'RideMotion' { return @('Join', 'MotionStand', 'MotionWalk', 'MotionJump', 'MotionReverse', 'MotionServerObserved') }
        'Scale' { return @('ScaleCount', 'ScaleClientLoad', 'SampleCompleted') }
    }
}

function Test-FiniteNumber {
    param($Value)
    $number = 0.0
    return $null -ne $Value -and
        [double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and
        -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)
}

function Get-CaseVerdict {
    param([object[]]$Events, [string[]]$Required, [string]$Mode)
    $errors = New-Object 'System.Collections.Generic.List[string]'
    $unsupported = @($Events | Where-Object { $_.status -eq 'UNSUPPORTED' -or (Get-PropertyValue $_ 'phase' '') -eq 'Unsupported' }).Count -gt 0
    foreach ($event in $Events) {
        if ($event.status -eq 'FAIL' -or ($event.status -eq 'ASSERT' -and (Get-PropertyValue $event 'passed' $false) -cne $true)) {
            $errors.Add(('Driver failure: {0} {1}' -f (Get-PropertyValue $event 'assertion' ''), (Get-PropertyValue $event 'detail' '')))
        }
    }
    foreach ($role in @('Server', 'Client', 'Partner')) {
        if (-not (Test-RoleDone -Events $Events -Role $role)) { $errors.Add("Missing successful DONE from $role") }
    }
    foreach ($name in $Required) {
        # 服务器自身通过不能代替远端确认；两端入场也必须分别有证据。
        $roles = if ($name -eq 'Join') { @('Server', 'Client') }
            elseif ($name -eq 'DedicatedClientState') { @('Partner', 'Client') }
            elseif ($name -like 'Partner*') { @('Partner') }
            elseif ($name -in @('ClientInputReady', 'GateOpen', 'GateClosed', 'ClientRide', 'ClientVictoryState', 'ClientRestartFailureRecovery', 'ClientRestart', 'ClientLeave', 'ConnectionFailureRecovered', 'ClientKeyHeld', 'ClientKeyInstalled', 'ClientKeyRepicked', 'LateJoinState', 'LateJoinKeyAttachments', 'ConnectionLostDetected', 'ReconnectStateRestored', 'ScaleClientLoad', 'MotionStand', 'MotionWalk', 'MotionJump', 'MotionReverse')) { @('Client') }
            else { @('Server') }
        foreach ($role in $roles) {
            $passes = @($Events | Where-Object { $_.status -eq 'ASSERT' -and (Get-PropertyValue $_ 'assertion' '') -eq $name -and (Get-PropertyValue $_ 'role' '') -ceq $role -and (Get-PropertyValue $_ 'passed' $false) -ceq $true })
            if ($passes.Count -eq 0) { $errors.Add("Missing passing assertion: $name from $role") }
        }
    }
    # 运动样本必须按角色与阶段保留，不能被最后一条服务器样本覆盖，也不能用规模测试指标代替。
    $motionMetrics = @($Events | Where-Object { $_.status -eq 'METRIC' -and (Get-PropertyValue (Get-PropertyValue $_ 'metrics') 'category' '') -ceq 'platformRide' })
    $samples = @($Events | Where-Object { $_.status -eq 'METRIC' -and (Get-PropertyValue $_ 'role' '') -eq 'Server' -and (Get-PropertyValue (Get-PropertyValue $_ 'metrics') 'category' '') -cne 'platformRide' })
    $metrics = $null
    if ($samples.Count -gt 0) { $metrics = Get-PropertyValue $samples[-1] 'metrics' }
    if ($Mode -eq 'Scale') {
        foreach ($field in @('outBytes', 'outPackets', 'sampleSeconds', 'actorCount')) {
            if ($null -eq (Get-PropertyValue $metrics $field)) { $errors.Add("Missing server metric: $field") }
        }
        if ([double](Get-PropertyValue $metrics 'sampleSeconds' 0) -le 0) { $errors.Add('Sampling duration must be positive') }
    }
    if ($Mode -eq 'RideMotion') {
        foreach ($role in @('Server', 'Client')) { foreach ($phase in @('Stand', 'Walk', 'Jump', 'Reverse')) {
            $phaseSamples = @($motionMetrics | Where-Object { (Get-PropertyValue $_ 'role' '') -ceq $role -and (Get-PropertyValue $_.metrics 'phase' '') -ceq $phase })
            if ($phaseSamples.Count -eq 0) { $errors.Add("Missing platformRide metric: $role/$phase"); continue }
            foreach ($sample in $phaseSamples) {
                foreach ($field in @('frameSamples', 'elapsedSeconds', 'basedSamples', 'fallingSamples', 'relativeTravelCm', 'maxRelativeStepCm', 'correctionCount', 'maxCorrectionCm', 'maxRiseCm', 'maxFrameSeconds', 'comparableCorrections', 'baseChangeCorrections', 'fallingBraking', 'platformNetHz')) {
                    $value = Get-PropertyValue $sample.metrics $field
                    if (-not (Test-FiniteNumber $value)) { $errors.Add("Invalid platformRide metric: $role/$phase/$field must be finite"); continue }
                    if ([double]$value -lt 0 -or ($field -in @('frameSamples', 'elapsedSeconds', 'platformNetHz') -and [double]$value -le 0)) {
                        $errors.Add("Invalid platformRide metric: $role/$phase/$field has no valid samples or is negative")
                    }
                }
                if ((Get-PropertyValue $sample.metrics 'landedAfterJump') -isnot [bool]) {
                    $errors.Add("Invalid platformRide metric: $role/$phase/landedAfterJump must be a boolean")
                }
                if ((Get-PropertyValue $sample.metrics 'platformNetHz') -ne $PlatformNetHz -or
                    (Get-PropertyValue $sample.metrics 'syncMode' '') -cne $PlatformSyncMode -or
                    (Get-PropertyValue $sample.metrics 'platformMotion' '') -cne $PlatformMotion) {
                    $errors.Add("Platform experiment configuration mismatch: $role/$phase")
                }
            }
        } }
    }
    $status = 'passed'
    if ($errors.Count -gt 0) { $status = 'failed' }
    if ($unsupported) { $status = 'unsupported' }
    return [pscustomobject]@{ status = $status; errors = @($errors.ToArray()); metrics = $metrics; motionMetrics = $motionMetrics }
}

function Get-CsvMetricSummary {
    param([string]$Text)
    # UE CSV 的尾部可含元数据；只统计表头对应列中的有限数值，绝不把缺列当作 0。
    $lines = @($Text -split '\r?\n')
    $headerIndex = -1
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match 'Exclusive/(GameThread/)?ServerReplicateActors') { $headerIndex = $index; break }
    }
    if ($headerIndex -lt 0) { return $null }
    $csvRows = @(($lines[$headerIndex..($lines.Count - 1)] -join "`n") | ConvertFrom-Csv)
    if ($csvRows.Count -eq 0) { return $null }
    $summary = [ordered]@{}
    foreach ($definition in @(
        @{ name = 'serverReplicateActors'; pattern = '^Exclusive/(GameThread/)?ServerReplicateActors$'; unit = 'ms'; required = $true },
        @{ name = 'gameThread'; pattern = '(^|/)GameThreadTime$'; unit = 'ms'; required = $false },
        @{ name = 'fullyDormantActors'; pattern = '^Replication/NumberOfFullyDormantActors$'; unit = 'actors'; required = $false },
        @{ name = 'replicateCallsPerConnection'; pattern = '^Replication/NumReplicateActorCallsPerConAvg$'; unit = 'calls/connection'; required = $false }
    )) {
        $columns = @($csvRows[0].PSObject.Properties.Name | Where-Object { $_.Trim().Trim([char]0xFEFF) -match $definition.pattern })
        if ($columns.Count -ne 1) {
            if ($definition.required) { return $null }
            $summary[$definition.name] = $null
            continue
        }
        $column = $columns[0]
        $values = New-Object 'System.Collections.Generic.List[double]'
        foreach ($row in $csvRows) {
            $number = 0.0
            if ([double]::TryParse([string]$row.$column, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number) -and $number -ge 0) { $values.Add($number) }
        }
        if ($values.Count -eq 0) {
            if ($definition.required) { return $null }
            $summary[$definition.name] = $null
            continue
        }
        $sorted = @($values.ToArray() | Sort-Object)
        $p95Index = [Math]::Max(0, [int][Math]::Ceiling($sorted.Count * 0.95) - 1)
        $summary[$definition.name] = [ordered]@{ sourceColumn = $column; unit = $definition.unit; sampleCount = $sorted.Count; nonZeroSampleCount = @($sorted | Where-Object { $_ -ne 0 }).Count; allZero = ($sorted[-1] -eq 0); mean = [Math]::Round(($sorted | Measure-Object -Average).Average, 6); p95 = [Math]::Round($sorted[$p95Index], 6); p95Method = 'nearest-rank'; max = $sorted[-1] }
    }
    return [pscustomobject]$summary
}

function Get-DisplayGameThreadMetric {
    param($CpuMetrics)
    # 保留 report 中的原始计数；计数器全零不代表游戏线程没有开销，展示与汇总应留空。
    if ($null -eq $CpuMetrics -or $null -eq $CpuMetrics.gameThread -or $CpuMetrics.gameThread.allZero) { return $null }
    return $CpuMetrics.gameThread
}

function Get-Median {
    param([double[]]$Values)
    if ($Values.Count -eq 0) { return $null }
    $ordered = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { return $ordered[$middle] }
    return ($ordered[$middle - 1] + $ordered[$middle]) / 2
}

if ($SelfTest) {
    foreach ($excluded in @('Source/.vs/index.db', 'Source\.git\objects\cache', 'Source/Binaries/game.exe', 'Tests/Intermediate/test.obj', 'Scripts/Saved/log.txt', 'Tests/Evidence/report.json', 'Source/Nested/.VS/index.db')) {
        if (Test-SourceManifestPath $excluded) { throw "Source manifest accepted generated/cache path: $excluded" }
    }
    foreach ($included in @('Source/multiplayer/Core/GameMode.cpp', 'Source/multiplayer/Save/SavedState.h', 'Config/DefaultGame.ini', 'Scripts/RunMultiplayerNetworkTests.ps1', 'Tests/EvidenceReader.cpp', 'Content/Config/Gameplay.json')) {
        if (-not (Test-SourceManifestPath $included)) { throw "Source manifest excluded source/config path: $included" }
    }
    $fixture = 'Log: COOP_TEST {"token":"current","mode":"Flow","role":"Server","status":"ASSERT","assertion":"Example","passed":true}' + "`n" +
        'Log: COOP_TEST {"token":"current","mode":"Flow","status":"DONE","role":"Server","passed":true}' + "`n" +
        'Log: COOP_TEST {"token":"current","mode":"Flow","status":"DONE","role":"Client","passed":true}'
    $fixture += "`n" + 'Log: COOP_TEST {"token":"current","mode":"Flow","role":"Partner","status":"DONE","passed":true}'
    $events = @(ConvertFrom-CoopLog $fixture -Token 'current' -Role 'Server' -Mode 'Flow') +
        @(ConvertFrom-CoopLog $fixture -Token 'current' -Role 'Client' -Mode 'Flow') +
        @(ConvertFrom-CoopLog $fixture -Token 'current' -Role 'Partner' -Mode 'Flow')
    if ((Get-CaseVerdict $events @('Example') 'Flow').status -ne 'passed') { throw 'Runner parser rejected valid fixture' }
    if ((Get-CaseVerdict $events @('Missing') 'Flow').status -ne 'failed') { throw 'Runner accepted a missing assertion' }
    if ((Get-CaseVerdict @($events | Where-Object { (Get-PropertyValue $_ 'role' '') -ne 'Client' }) @('Example') 'Flow').status -ne 'failed') { throw 'Runner accepted a missing peer' }
    $oldFixture = $fixture.Replace('"current"', '"previous"') + "`n" +
        'Log: COOP_TEST {"token":"previous","mode":"Flow","role":"Server","status":"READY"}' + "`n" +
        'Log: COOP_TEST {"token":"previous","mode":"Flow","role":"Server","status":"METRIC","metrics":{"outBytes":123}}'
    $oldEvents = @(ConvertFrom-CoopLog $oldFixture -Token 'current' -Role 'Server' -Mode 'Flow') +
        @(ConvertFrom-CoopLog $oldFixture -Token 'current' -Role 'Client' -Mode 'Flow')
    if ($oldEvents.Count -ne 0 -or (Get-CaseVerdict $oldEvents @('Example') 'Flow').status -ne 'failed') { throw 'Runner accepted READY/DONE/ASSERT/METRIC from a previous run' }
    $mixedEvents = @(ConvertFrom-CoopLog ($oldFixture + "`n" + $fixture) -Token 'current' -Role 'Server' -Mode 'Flow')
    if ($mixedEvents.Count -ne 2 -or (Get-CaseVerdict $mixedEvents @('Example') 'Flow').status -ne 'failed') { throw 'Runner accepted an old event or a wrong-role peer completion' }
    $doneOnlyFixture = ($fixture -split "`n" | Where-Object { $_ -notmatch '"ASSERT"' }) -join "`n"
    $missingCurrentAssertion = @(ConvertFrom-CoopLog ($oldFixture + "`n" + $doneOnlyFixture) -Token 'current' -Role 'Server' -Mode 'Flow') +
        @(ConvertFrom-CoopLog ($oldFixture + "`n" + $doneOnlyFixture) -Token 'current' -Role 'Client' -Mode 'Flow')
    if ((Get-CaseVerdict $missingCurrentAssertion @('Example') 'Flow').status -ne 'failed') { throw 'Previous passing assertion incorrectly completed current DONE events' }
    if (@(ConvertFrom-CoopLog $fixture -Token 'current' -Role 'Server' -Mode 'Scale').Count -ne 0) { throw 'Runner accepted events from a different scenario' }
    $wrongRoleEvents = @(ConvertFrom-CoopLog ($fixture.Replace('"Example"', '"ScaleClientLoad"')) -Token 'current' -Role 'Server' -Mode 'Flow') +
        @($events | Where-Object { $_.role -eq 'Client' })
    if ((Get-CaseVerdict $wrongRoleEvents @('ScaleClientLoad') 'Flow').status -ne 'failed') { throw 'Server assertion incorrectly satisfied a client-only load check' }
    foreach ($clientAssertion in @('ClientInputReady', 'ClientRestartFailureRecovery', 'ConnectionFailureRecovered')) {
        $clientEvents = @($events | Where-Object status -eq 'DONE') +
            @([pscustomobject]@{ status = 'ASSERT'; role = 'Client'; assertion = $clientAssertion; passed = $true })
        if ((Get-CaseVerdict $clientEvents @($clientAssertion) 'Flow').status -ne 'passed') { throw "Client assertion rejected: $clientAssertion" }
        $clientEvents[-1].role = 'Server'
        if ((Get-CaseVerdict $clientEvents @($clientAssertion) 'Flow').status -ne 'failed') { throw "Server incorrectly satisfied client assertion: $clientAssertion" }
    }
    $smokeEvents = @($events | Where-Object status -eq 'DONE')
    foreach ($role in @('Partner', 'Client')) {
        $smokeEvents += [pscustomobject]@{ status = 'ASSERT'; role = $role; assertion = 'DedicatedClientState'; passed = $true }
    }
    if ((Get-CaseVerdict $smokeEvents @('DedicatedClientState') 'DedicatedSmoke').status -ne 'passed') { throw 'Two independent smoke client receipts rejected' }
    foreach ($role in @('Partner', 'Client')) {
        $missing = @($smokeEvents | Where-Object { -not ($_.status -eq 'ASSERT' -and $_.role -eq $role) })
        if ((Get-CaseVerdict $missing @('DedicatedClientState') 'DedicatedSmoke').status -ne 'failed') { throw "Smoke accepted missing $role receipt" }
    }
    # 构造完整的双端四阶段日志，再逐项破坏证据，防止“只有 DONE”或服务器代答造成假通过。
    $motionLines = @()
    foreach ($role in @('Server', 'Client')) {
        $assertions = if ($role -eq 'Server') { @('Join', 'MotionServerObserved') } else { @('Join', 'MotionStand', 'MotionWalk', 'MotionJump', 'MotionReverse') }
        foreach ($assertion in $assertions) {
            $motionLines += 'Log: COOP_TEST ' + ([ordered]@{ token = 'motion'; mode = 'RideMotion'; role = $role; status = 'ASSERT'; assertion = $assertion; passed = $true } | ConvertTo-Json -Compress)
        }
        foreach ($phase in @('Stand', 'Walk', 'Jump', 'Reverse')) {
            $motionLines += 'Log: COOP_TEST ' + ([ordered]@{
                token = 'motion'; mode = 'RideMotion'; role = $role; status = 'METRIC'
                metrics = @{ category = 'platformRide'; phase = $phase; frameSamples = 120; elapsedSeconds = 2.0; basedSamples = 100; fallingSamples = 20; relativeTravelCm = 50.0; maxRelativeStepCm = 2.0; correctionCount = 0; maxCorrectionCm = 0.0; maxRiseCm = 50.0; maxFrameSeconds = 0.02; comparableCorrections = 0; baseChangeCorrections = 0; fallingBraking = 0; landedAfterJump = ($phase -eq 'Jump'); platformNetHz = $PlatformNetHz; syncMode = $PlatformSyncMode; platformMotion = $PlatformMotion }
            } | ConvertTo-Json -Depth 4 -Compress)
        }
        $motionLines += 'Log: COOP_TEST ' + ([ordered]@{ token = 'motion'; mode = 'RideMotion'; role = $role; status = 'DONE'; passed = $true } | ConvertTo-Json -Compress)
    }
    $motionLines += 'Log: COOP_TEST {"token":"motion","mode":"RideMotion","role":"Partner","status":"DONE","passed":true}'
    $motionFixture = $motionLines -join "`n"
    $motionEvents = @(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Server' -Mode 'RideMotion') +
        @(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Client' -Mode 'RideMotion') +
        @(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Partner' -Mode 'RideMotion')
    $motionRequired = @(Get-RequiredAssertions 'RideMotion')
    $motionVerdict = Get-CaseVerdict $motionEvents $motionRequired 'RideMotion'
    if ($motionVerdict.status -ne 'passed' -or $motionVerdict.motionMetrics.Count -ne 8 -or $null -ne $motionVerdict.metrics) { throw 'Runner did not preserve valid per-role motion measurements' }
    foreach ($role in @('Server', 'Client')) { foreach ($phase in @('Stand', 'Walk', 'Jump', 'Reverse')) {
        $incomplete = @($motionEvents | Where-Object { -not ($_.status -eq 'METRIC' -and $_.role -eq $role -and $_.metrics.phase -eq $phase) })
        if ((Get-CaseVerdict $incomplete $motionRequired 'RideMotion').status -ne 'failed') { throw "Runner accepted missing motion sample: $role/$phase" }
    } }
    foreach ($assertion in @('Join', 'MotionStand', 'MotionWalk', 'MotionJump', 'MotionReverse')) {
        $wrongRole = $motionEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        foreach ($event in $wrongRole) {
            if ($event.status -eq 'ASSERT' -and $event.role -eq 'Client' -and $event.assertion -eq $assertion) { $event.role = 'Server' }
        }
        if ((Get-CaseVerdict $wrongRole $motionRequired 'RideMotion').status -ne 'failed') { throw "Server incorrectly satisfied client assertion: $assertion" }
    }
    foreach ($invalidCase in @(
        @{ field = 'frameSamples'; value = 0 }, @{ field = 'elapsedSeconds'; value = 0 },
        @{ field = 'maxCorrectionCm'; value = 'NaN' }, @{ field = 'maxRelativeStepCm'; value = 'Infinity' },
        @{ field = 'relativeTravelCm'; value = $null }, @{ field = 'correctionCount'; value = -1 },
        @{ field = 'platformNetHz'; value = 0 }, @{ field = 'syncMode'; value = 'Unknown' },
        @{ field = 'platformMotion'; value = 'Unknown' },
        @{ field = 'maxRiseCm'; value = 'NaN' }, @{ field = 'maxRiseCm'; value = -1 },
        @{ field = 'maxFrameSeconds'; value = 'Infinity' }, @{ field = 'maxFrameSeconds'; value = -1 },
        @{ field = 'comparableCorrections'; value = $null }, @{ field = 'comparableCorrections'; value = -1 },
        @{ field = 'baseChangeCorrections'; value = 'NaN' }, @{ field = 'baseChangeCorrections'; value = -1 },
        @{ field = 'fallingBraking'; value = 'NaN' }, @{ field = 'fallingBraking'; value = -1 },
        @{ field = 'landedAfterJump'; value = $null }, @{ field = 'landedAfterJump'; value = 'false' },
        @{ field = 'landedAfterJump'; value = 0 },
        @{ field = 'category'; value = 'scale' }
    )) {
        $invalidEvents = $motionEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        $invalidMetric = @($invalidEvents | Where-Object { $_.status -eq 'METRIC' -and $_.role -eq 'Client' })[0]
        $invalidMetric.metrics.($invalidCase.field) = $invalidCase.value
        if ((Get-CaseVerdict $invalidEvents $motionRequired 'RideMotion').status -ne 'failed') { throw "Runner accepted invalid motion field: $($invalidCase.field)" }
    }
    $withoutMotion = @($motionEvents | Where-Object { $_.status -ne 'METRIC' }) +
        @([pscustomobject]@{ token = 'motion'; mode = 'RideMotion'; role = 'Server'; status = 'METRIC'; metrics = [pscustomobject]@{ outBytes = 123; sampleSeconds = 2; actorCount = 1; outPackets = 2 } })
    if ((Get-CaseVerdict $withoutMotion $motionRequired 'RideMotion').status -ne 'failed') { throw 'Scale metrics incorrectly replaced motion measurements' }
    if ((Get-Median @(1, 3, 2)) -ne 2 -or (Get-Median @(1, 4)) -ne 2.5) { throw 'Invalid median' }
    $csvFixture = "FrameTime,GameThreadTime,Exclusive/ServerReplicateActors`n16,4,1`n17,6,3`n[Metadata],not-a-number,not-a-number"
    $cpu = Get-CsvMetricSummary $csvFixture
    if ($null -eq $cpu -or $cpu.gameThread.mean -ne 5 -or $cpu.serverReplicateActors.p95 -ne 3) { throw 'CSV mean/P95 self-test failed' }
    if ($null -ne (Get-CsvMetricSummary "FrameTime,GameThreadTime`n16,4")) { throw 'Missing CPU column incorrectly became a measurement' }
    $dsCpu = Get-CsvMetricSummary "FrameTime,Exclusive/GameThread/ServerReplicateActors`n33,1`n34,3"
    if ($null -eq $dsCpu -or $dsCpu.serverReplicateActors.mean -ne 2 -or $null -ne $dsCpu.gameThread -or $null -ne (Get-DisplayGameThreadMetric $dsCpu)) { throw 'DS CSV must preserve replication CPU and leave missing total GT unavailable' }
    if ($null -ne (Get-CsvMetricSummary "FrameTime,Exclusive/GameThread/ServerReplicateActors`n33,NaN`n34,invalid")) { throw 'Invalid DS replication samples must not pass' }
    $zeroCpu = Get-CsvMetricSummary "FrameTime,GameThreadTime,Exclusive/ServerReplicateActors`n16,0,1`n17,0,3"
    if ($null -ne (Get-DisplayGameThreadMetric $zeroCpu) -or $zeroCpu.gameThread.mean -ne 0 -or -not $zeroCpu.gameThread.allZero) { throw 'All-zero GameThread counter was displayed as cost or raw evidence was changed' }
    if ($null -ne (Get-DisplayGameThreadMetric $null) -or (Get-DisplayGameThreadMetric $cpu).mean -ne 5) { throw 'Valid/missing GameThread display metric handling failed' }
    Write-Output 'Runner contract self-test passed. No Unreal process or network test was run.'
    return
}

if ($GameExecutablePath -and -not $ServerExecutablePath) { throw 'Packaged DS tests require a separate ServerExecutablePath.' }
if ($ServerExecutablePath -and -not (Test-Path -LiteralPath $ServerExecutablePath)) { throw 'Server executable not found' }
$ExecutablePath = $EditorPath
$BuildKind = 'editor-game'
if ($GameExecutablePath) {
    $ExecutablePath = [IO.Path]::GetFullPath($GameExecutablePath)
    $BuildKind = 'development-game'
    $binaryDirectory = Split-Path -Parent $ExecutablePath
    $isGameBinary = (Split-Path -Leaf $binaryDirectory) -eq 'Win64' -and
        (Split-Path -Leaf (Split-Path -Parent $binaryDirectory)) -eq 'Binaries'
    if (-not $isGameBinary) {
        # 打包根目录的同名 exe 只是启动器；直接启动真正进程，PID 清理和文件指纹才对应被测游戏。
        $projectName = [IO.Path]::GetFileNameWithoutExtension($ProjectPath)
        $gameBinary = Join-Path $binaryDirectory "$projectName/Binaries/Win64/$projectName.exe"
        if (Test-Path -LiteralPath $gameBinary -PathType Leaf) { $ExecutablePath = [IO.Path]::GetFullPath($gameBinary) }
        elseif (-not $PlanOnly -or (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) {
            throw 'GameExecutablePath must identify the actual packaged Binaries/Win64 game executable, not an unresolved bootstrap launcher.'
        }
    }
}
if (-not $PlanOnly -and -not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) { throw "Executable not found: $ExecutablePath" }
if (-not (Test-Path -LiteralPath $ProjectPath -PathType Leaf)) { throw "Project not found: $ProjectPath" }
$RunId = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $ProjectRoot "Saved/NetworkValidation/$RunId" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$sourceManifest = @(Get-SourceManifest)
$fingerprint = Get-StringHash (($sourceManifest | ForEach-Object { $_.path + "`t" + $_.sha256 }) -join "`n")
$gitCommit = (& git -C $ProjectRoot rev-parse HEAD 2>$null | Out-String).Trim()
$gitStatus = @(& git -C $ProjectRoot status --porcelain 2>$null)
$configPath = Join-Path $ProjectRoot 'Content/Config/Gameplay.json'
$configText = if (Test-Path -LiteralPath $configPath) { [IO.File]::ReadAllText($configPath) } else { $null }
$configHash = if ($null -ne $configText) { (Get-FileHash -LiteralPath $configPath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
$projectDescription = [IO.File]::ReadAllText($ProjectPath) | ConvertFrom-Json
$exeVersion = if (Test-Path -LiteralPath $ExecutablePath) { (Get-Item -LiteralPath $ExecutablePath).VersionInfo.FileVersion } else { $null }
$exeHash = if (Test-Path -LiteralPath $ExecutablePath -PathType Leaf) { (Get-FileHash -LiteralPath $ExecutablePath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
# 编辑器进程运行的是项目 DLL；只记录引擎 exe 无法区分两次不同的玩法构建。
$projectModule = $null
if ($BuildKind -eq 'editor-game') {
    $projectModulePath = Join-Path $ProjectRoot 'Binaries/Win64/UnrealEditor-multiplayer.dll'
    $projectModuleHash = if (Test-Path -LiteralPath $projectModulePath -PathType Leaf) { (Get-FileHash -LiteralPath $projectModulePath -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null }
    $projectModule = [ordered]@{ path = $projectModulePath; sha256 = $projectModuleHash; resolution = 'editor-project-module-on-disk'; loadedByRuntimeVerified = $false }
}
# 源工程表和被测打包表分别留证据。定位不到时保留 null，不能拿源表冒充打包内容。
$runtimeConfig = [ordered]@{ path = $null; sha256 = $null; matchesSource = $null; resolution = 'unresolved'; loadedByRuntimeVerified = $false }
if ($BuildKind -eq 'editor-game') {
    $runtimeConfig.path = $configPath
    $runtimeConfig.sha256 = $configHash
    $runtimeConfig.matchesSource = $(if ($configHash) { $true } else { $null })
    $runtimeConfig.resolution = 'editor-project-content'
} elseif ($exeHash) {
    $binaryDirectory = Split-Path -Parent $ExecutablePath
    if ((Split-Path -Leaf $binaryDirectory) -eq 'Win64' -and (Split-Path -Leaf (Split-Path -Parent $binaryDirectory)) -eq 'Binaries') {
        $packageProjectRoot = Split-Path -Parent (Split-Path -Parent $binaryDirectory)
        $packageConfigPath = Join-Path $packageProjectRoot 'Content/Config/Gameplay.json'
        $runtimeConfig.path = $packageConfigPath
        $runtimeConfig.resolution = 'packaged-loose-file-missing-or-inside-container'
        if (Test-Path -LiteralPath $packageConfigPath -PathType Leaf) {
            $runtimeConfig.sha256 = (Get-FileHash -LiteralPath $packageConfigPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $runtimeConfig.matchesSource = $(if ($configHash) { $runtimeConfig.sha256 -ceq $configHash } else { $null })
            $runtimeConfig.resolution = 'packaged-loose-file-derived-from-game-binary'
        }
    }
}
$report = [ordered]@{
    schemaVersion = 1; runId = $RunId; generatedAtUtc = [DateTime]::UtcNow.ToString('o'); status = 'running'; planOnly = [bool]$PlanOnly
    environment = [ordered]@{ machine = $env:COMPUTERNAME; engineAssociation = $projectDescription.EngineAssociation; executable = $ExecutablePath; executableVersion = $exeVersion; executableSha256 = $exeHash; projectModule = $projectModule; runtimeGameplayConfig = $runtimeConfig; buildKind = $BuildKind; map = $Map; nullRhi = (-not ($Visible -or $Offscreen)); offscreenDx11 = [bool]$Offscreen; topology = 'One local dedicated server and two separate remote client processes'; visualValidation = $false }
    provenance = [ordered]@{ gitCommit = $gitCommit; dirty = ($gitStatus.Count -gt 0); gitStatus = $gitStatus; sourceFingerprintSha256 = $fingerprint; sourceManifest = $sourceManifest; gameplayConfigPath = $configPath; gameplayConfigSha256 = $configHash; gameplayConfigText = $configText }
    scope = @('Assertions represent the driver checks, not manual visual acceptance.', 'PktLag values are emulator parameters applied to all three peers, not measured RTT.', 'Scale tests use synthetic actors and three processes, not player-capacity certification.', 'RideMotion retains per-role, per-phase runtime measurements; they do not certify rendered smoothness.', 'Historical 22.17% bandwidth data is not a result of this run.', 'Provenance gameplayConfig fields describe source files; environment.runtimeGameplayConfig describes the located runtime file, not proof that the engine loaded it.')
    cases = @(); summaries = @()
}
$cases = New-Object 'System.Collections.Generic.List[object]'
$profileSettings = @{
    Normal = @{ lag = 0; variance = 0; loss = 0 }
    Moderate = @{ lag = 100; variance = 20; loss = 2 }
    Harsh = @{ lag = 200; variance = 50; loss = 5 }
}
if ($Suite -in @('Regression', 'All')) {
    $modes = if ($Scenario -eq 'All') { @('Flow', 'Keys', 'LateJoin', 'Reconnect', 'ConnectionRetry', 'Ride', 'RideMotion') } else { @($Scenario) }
    foreach ($mode in $modes) { foreach ($profile in $Profiles) { for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
        $cases.Add([pscustomobject]@{ mode = $mode; matrix = 'Regression'; profile = $profile; iteration = $iteration; staticCount = 0; movingCount = 1; optimization = 'Combined' })
    } } }
}
if ($Suite -in @('Scale', 'All')) {
    if ($Matrix -in @('Static', 'All')) {
        foreach ($static in $StaticCounts) { foreach ($optimization in @('Baseline', 'Dormancy')) { for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
            $cases.Add([pscustomobject]@{ mode = 'Scale'; matrix = 'Static'; profile = 'Normal'; iteration = $iteration; staticCount = $static; movingCount = 0; optimization = $optimization })
        } } }
    }
    if ($Matrix -in @('Moving', 'All')) {
        foreach ($moving in $MovingCounts) { foreach ($optimization in @('Baseline', 'Frequency')) { for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
            $cases.Add([pscustomobject]@{ mode = 'Scale'; matrix = 'Moving'; profile = 'Normal'; iteration = $iteration; staticCount = 0; movingCount = $moving; optimization = $optimization })
        } } }
    }
}

function Save-Report {
    $report.generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'report.json'), ($report | ConvertTo-Json -Depth 30), $Utf8)
}

function Start-TestPeer {
    param([string]$ArgumentLine, [string]$Role = 'Client')
    $peerExecutable = if ($Role -eq 'Server' -and $ServerExecutablePath) { $ServerExecutablePath } else { $ExecutablePath }
    $start = @{ FilePath = $peerExecutable; ArgumentList = $ArgumentLine; WorkingDirectory = $ProjectRoot; PassThru = $true }
    if (-not $Visible -or $Role -eq 'Server') { $start.WindowStyle = 'Hidden' }
    return Start-Process @start
}

try {
    $caseIndex = 0
    foreach ($case in $cases) {
        $caseIndex++
        $caseId = '{0:D3}-{1}-{2}-{3}-{4}-{5}-{6}' -f $caseIndex, $case.mode, $case.profile, $case.staticCount, $case.movingCount, $case.optimization, $case.iteration
        $port = if ($ListenPort -gt 0) { $ListenPort } else { Get-FreeUdpPort }
        $token = "$RunId-$caseIndex"
        $result = [ordered]@{ id = $caseId; token = $token; mode = $case.mode; matrix = $case.matrix; profile = $case.profile; iteration = $case.iteration; staticCount = $case.staticCount; movingCount = $case.movingCount; optimization = $case.optimization; warmupSeconds = $(if ($case.mode -ne 'RideMotion') { $WarmupSeconds } else { $null }); sampleSeconds = $(if ($case.mode -ne 'RideMotion') { $SampleSeconds } else { $null }); samplingDurationSource = $(if ($case.mode -eq 'RideMotion') { 'motionMetrics[].metrics.elapsedSeconds, separately measured per role and phase' } else { 'metrics.sampleSeconds where available' }); outageModel = $(if ($case.mode -eq 'Reconnect') { 'primary-server-outbound-drop-until-client-timeout' } else { $null }); platformNetHz = $(if ($case.mode -eq 'RideMotion') { $PlatformNetHz } else { $null }); platformSyncMode = $(if ($case.mode -eq 'RideMotion') { $PlatformSyncMode } else { $null }); port = $port; startedAtUtc = [DateTime]::UtcNow.ToString('o'); durationSeconds = 0; status = 'planned'; requiredAssertions = @('DedicatedAuthority') + @(Get-RequiredAssertions $case.mode); commands = @(); peers = @(); events = @(); errors = @(); metrics = $null; motionMetrics = @(); csvPath = $null; csvSha256 = $null; cpuMetrics = $null; measurementWarnings = @() }
        if ($case.mode -ne 'DedicatedSmoke') { $result.requiredAssertions += 'PartnerReady' }
        $result['platformMotion'] = $(if ($case.mode -eq 'RideMotion') { $PlatformMotion } else { $null })
        $network = $profileSettings[$case.profile]
        $peerSpecs = @{}
        foreach ($role in @('Server', 'Partner', 'Client')) {
            $logPath = Join-Path $OutputDirectory "$caseId-$role.log"
            $url = if ($role -eq 'Server') { $Map }
                elseif ($case.mode -eq 'DedicatedSmoke') { "127.0.0.1:$port" }
                else { '/Game/UI/DSMenu' }
            $arguments = @()
            if ($BuildKind -eq 'editor-game') { $arguments += $ProjectPath }
            $arguments += @($url, '-game', '-NoSplash', '-NoSound', '-unattended', '-DDC=InstalledNoZenLocalFallback', "-abslog=$logPath", "-port=$port", "-CoopTestToken=$token", "-CoopNetTest=$($case.mode)", "-CoopTestRole=$role", "-CoopStaticCount=$($case.staticCount)", "-CoopMovingCount=$($case.movingCount)", "-CoopOptimization=$($case.optimization)", "-CoopWarmupSeconds=$WarmupSeconds", "-CoopSampleSeconds=$SampleSeconds", "-PktLag=$($network.lag)", "-PktLagVariance=$($network.variance)", "-PktLoss=$($network.loss)")
            if ($role -eq 'Server') { $arguments += '-server' }
            $arguments += @("-CoopServerAddress=127.0.0.1:$port")
            if (($Visible -or $Offscreen) -and $role -ne 'Server') { $arguments += @('-windowed', '-ResX=960', '-ResY=540') } else { $arguments += '-NullRHI' }
            # 离屏 DX11 验收走真实渲染初始化，窗口仍隐藏；不能与 NullRHI 的性能数据混算。
            if ($Offscreen -and $role -ne 'Server') { $arguments += @('-RenderOffscreen', '-dx11') }
            $arguments += @('-DDC-ForceMemoryCache', '-NoVSync', '-ExecCmds=t.MaxFPS 60', "-CoopTestTimeout=$TimeoutSeconds", "-CoopMatrix=$(Get-PropertyValue $case 'matrix' '')", "-CoopCsvPath=$(Join-Path $OutputDirectory "$caseId-$role.csv")")
            if ($case.mode -eq 'Flow' -and $role -eq 'Server') { $arguments += '-CoopTestFailRestartOnce' }
            if ($case.mode -eq 'RideMotion') { $arguments += @("-CoopPlatformNetHz=$PlatformNetHz", "-CoopPlatformSyncMode=$PlatformSyncMode", "-CoopPlatformMotion=$PlatformMotion") }
            $argumentLine = ($arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
            $peerSpecs[$role] = [pscustomobject]@{ role = $role; log = $logPath; arguments = $argumentLine }
            $commandExecutable = if ($role -eq 'Server' -and $ServerExecutablePath) { $ServerExecutablePath } else { $ExecutablePath }
            $result.commands += [ordered]@{ role = $role; executable = $commandExecutable; arguments = $arguments; rendered = (Quote-Argument $commandExecutable) + ' ' + $argumentLine }
        }
        $report.cases += $result
        if ($PlanOnly) { continue }
        Write-Host "[$caseIndex/$($cases.Count)] $caseId"
        $startedProcesses = New-Object 'System.Collections.Generic.List[object]'
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $failure = $null
        $finished = $false
        try {
            $serverProcess = Start-TestPeer $peerSpecs.Server.arguments -Role 'Server'
            $startedProcesses.Add([pscustomobject]@{ role = 'Server'; process = $serverProcess; log = $peerSpecs.Server.log })
            $ready = $false
            while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
                $serverEvents = @(Get-CoopEvents $peerSpecs.Server.log -Token $token -Role 'Server' -Mode $case.mode)
                if (@($serverEvents | Where-Object { $_.status -eq 'READY' -and (Get-PropertyValue $_ 'role' '') -eq 'Server' }).Count -gt 0) { $ready = $true; break }
                if (@($serverEvents | Where-Object { $_.status -in @('FAIL', 'UNSUPPORTED', 'DONE') }).Count -gt 0) { break }
                $serverProcess.Refresh()
                if ($serverProcess.HasExited) { break }
                Start-Sleep -Milliseconds 250
            }
            if (-not $ready) { throw 'Server did not emit READY before failure, exit or timeout' }
            $partnerProcess = Start-TestPeer $peerSpecs.Partner.arguments
            $startedProcesses.Add([pscustomobject]@{ role = 'Partner'; process = $partnerProcess; log = $peerSpecs.Partner.log })
            # 冒烟只检查两个真实远端同时入场；其他场景须先让 Partner 准备测试状态。
            $primaryReady = $case.mode -eq 'DedicatedSmoke'
            while (-not $primaryReady -and $watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
                $serverEvents = @(Get-CoopEvents $peerSpecs.Server.log -Token $token -Role 'Server' -Mode $case.mode)
                if (@($serverEvents | Where-Object status -eq 'PRIMARY_READY').Count) { $primaryReady = $true; break }
                if ($serverProcess.HasExited -or $partnerProcess.HasExited -or @($serverEvents | Where-Object status -eq 'FAIL').Count) { break }
                Start-Sleep -Milliseconds 250
            }
            if (-not $primaryReady) { throw 'Partner did not become ready before primary connection' }
            $clientProcess = Start-TestPeer $peerSpecs.Client.arguments
            $startedProcesses.Add([pscustomobject]@{ role = 'Client'; process = $clientProcess; log = $peerSpecs.Client.log })
            while ($watch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
                $events = @(Get-CoopEvents $peerSpecs.Server.log -Token $token -Role 'Server' -Mode $case.mode) +
                    @(Get-CoopEvents $peerSpecs.Client.log -Token $token -Role 'Client' -Mode $case.mode) +
                    @(Get-CoopEvents $peerSpecs.Partner.log -Token $token -Role 'Partner' -Mode $case.mode)
                if (@($events | Where-Object { $_.status -in @('FAIL', 'UNSUPPORTED') }).Count -gt 0) { break }
                if ((Test-RoleDone $events 'Server') -and (Test-RoleDone $events 'Client') -and (Test-RoleDone $events 'Partner')) { $finished = $true; break }
                $serverProcess.Refresh(); $clientProcess.Refresh()
                if (($partnerProcess.HasExited -and -not (Test-RoleDone $events 'Partner')) -or ($serverProcess.HasExited -and -not (Test-RoleDone $events 'Server')) -or ($clientProcess.HasExited -and -not (Test-RoleDone $events 'Client'))) { break }
                Start-Sleep -Milliseconds 250
            }
            if (-not $finished) { $failure = 'All three peers did not finish successfully before failure, exit or timeout' }
        }
        catch { $failure = $_.Exception.Message }
        finally {
            # 只清理本例确实创建的进程对象，不按名称全局结束 UE。
            foreach ($peer in $startedProcesses) {
                $process = $peer.process
                try {
                    # DONE 只表示玩法检查完成，不能掩盖随后退出时崩溃；给驱动的正常退出留有限时间。
                    if ($finished) { [void]$process.WaitForExit(10000) }
                    $process.Refresh()
                    $forced = -not $process.HasExited
                    if ($forced) { $process.Kill(); [void]$process.WaitForExit(5000) }
                    $exitCode = if ($process.HasExited) { $process.ExitCode } else { $null }
                    $result.peers += [ordered]@{ role = $peer.role; pid = $process.Id; exitCode = $exitCode; stoppedByRunner = $forced; log = $peer.log }
                    if (-not $forced -and $exitCode -ne 0) { $result.errors += "$($peer.role) exited with code $exitCode" }
                    if ($finished -and $forced) { $result.errors += "$($peer.role) did not exit cleanly after DONE" }
                    if ((Read-SharedText $peer.log) -match '(?im)(?:Fatal error:|Assertion failed:|=== Critical error: ===)') {
                        $result.errors += "$($peer.role) log contains a fatal error or assertion"
                    }
                } catch { $result.errors += "Could not clean up owned process $($process.Id): $($_.Exception.Message)" }
            }
            $watch.Stop()
            $result.durationSeconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3)
            $result.events = @(Get-CoopEvents $peerSpecs.Server.log -Token $token -Role 'Server' -Mode $case.mode) +
                @(Get-CoopEvents $peerSpecs.Client.log -Token $token -Role 'Client' -Mode $case.mode) +
                    @(Get-CoopEvents $peerSpecs.Partner.log -Token $token -Role 'Partner' -Mode $case.mode)
            $verdict = Get-CaseVerdict $result.events $result.requiredAssertions $case.mode
            $result.status = $verdict.status
            $result.errors += $verdict.errors
            $result.metrics = $verdict.metrics
            $result.motionMetrics = $verdict.motionMetrics
            if ($case.mode -eq 'Scale') {
                # 只读取驱动真实上报的文件；找不到时保留 null 并判失败，不能以计划路径捏造结果。
                $result.csvPath = Get-PropertyValue $verdict.metrics 'csvPath'
                if (-not $result.csvPath) {
                    $metricEvents = @($result.events | Where-Object { $_.status -eq 'METRIC' -and (Get-PropertyValue $_ 'role' '') -eq 'Server' })
                    if ($metricEvents.Count) { $result.csvPath = Get-PropertyValue $metricEvents[-1] 'csvPath' }
                }
                try {
                    if ($result.csvPath -and (Test-Path -LiteralPath $result.csvPath -PathType Leaf)) {
                        $result.cpuMetrics = Get-CsvMetricSummary (Read-SharedText $result.csvPath)
                        $result.csvSha256 = (Get-FileHash -LiteralPath $result.csvPath -Algorithm SHA256).Hash.ToLowerInvariant()
                    }
                } catch { $result.errors += "CPU CSV parsing failed: $($_.Exception.Message)" }
                if ($null -eq $result.cpuMetrics) {
                    $result.errors += 'Missing valid measured CSV column: Exclusive/GameThread/ServerReplicateActors'
                    if ($result.status -ne 'unsupported') { $result.status = 'failed' }
                } elseif ($null -eq $result.cpuMetrics.gameThread) {
                    $result.measurementWarnings += 'GameThreadTime is unavailable in this DS CSV; total game-thread cost remains null. Replication CPU is measured separately.'
                } elseif ($result.cpuMetrics.gameThread.allZero) {
                    $result.measurementWarnings += 'GameThreadTime contains only raw zeros; do not interpret this as zero CPU cost. The engine counter may be unavailable in this configuration.'
                }
            }
            if ($failure) { $result.errors += $failure }
            if ($result.errors.Count -gt 0 -and $result.status -ne 'unsupported') { $result.status = 'failed' }
            Save-Report
        }
    }
}
finally {
    $rows = @($report.cases | ForEach-Object {
        $m = $_.metrics
        $seconds = [double](Get-PropertyValue $m 'sampleSeconds' 0)
        $bytes = Get-PropertyValue $m 'outBytes'
        $cpu = $_.cpuMetrics
        $displayGameThread = Get-DisplayGameThreadMetric $cpu
        [pscustomobject]@{
            id = $_.id; mode = $_.mode; matrix = $_.matrix; profile = $_.profile; iteration = $_.iteration
            staticCount = $_.staticCount; movingCount = $_.movingCount; optimization = $_.optimization; status = $_.status
            platformNetHz = $_.platformNetHz; platformSyncMode = $_.platformSyncMode; platformMotion = $_.platformMotion
            sampleSeconds = $(if ($seconds -gt 0) { $seconds } else { $null }); outBytes = $bytes; outPackets = (Get-PropertyValue $m 'outPackets')
            bytesPerSecond = $(if ($seconds -gt 0 -and $null -ne $bytes) { [Math]::Round([double]$bytes / $seconds, 3) } else { $null })
            serverReplicateMeanMs = $(if ($null -ne $cpu) { $cpu.serverReplicateActors.mean } else { $null })
            serverReplicateP95Ms = $(if ($null -ne $cpu) { $cpu.serverReplicateActors.p95 } else { $null })
            gameThreadMeanMs = $(if ($null -ne $displayGameThread) { $displayGameThread.mean } else { $null })
            gameThreadP95Ms = $(if ($null -ne $displayGameThread) { $displayGameThread.p95 } else { $null })
            gameThreadAllZero = $(if ($null -ne $cpu -and $null -ne $cpu.gameThread) { $cpu.gameThread.allZero } else { $null })
            fullyDormantActorsMean = $(if ($null -ne $cpu -and $null -ne $cpu.fullyDormantActors) { $cpu.fullyDormantActors.mean } else { $null })
            replicateCallsPerConnectionMean = $(if ($null -ne $cpu -and $null -ne $cpu.replicateCallsPerConnection) { $cpu.replicateCallsPerConnection.mean } else { $null })
            errors = ($_.errors -join ' | '); measurementWarnings = ($_.measurementWarnings -join ' | ')
        }
    })
    foreach ($group in @($rows | Group-Object mode, matrix, profile, staticCount, movingCount, optimization, platformNetHz, platformSyncMode, platformMotion)) {
        $valid = @($group.Group | Where-Object { $_.status -eq 'passed' -and $null -ne $_.bytesPerSecond })
        $values = @($valid | ForEach-Object { [double]$_.bytesPerSecond })
        $cpuValid = @($valid | Where-Object { $null -ne $_.serverReplicateMeanMs -and $null -ne $_.serverReplicateP95Ms })
        $gameThreadValid = @($valid | Where-Object { $null -ne $_.gameThreadMeanMs -and $null -ne $_.gameThreadP95Ms })
        $dormantValid = @($valid | Where-Object { $null -ne $_.fullyDormantActorsMean })
        $report.summaries += [ordered]@{
            group = $group.Name; matrix = $group.Group[0].matrix; attempted = $group.Count
            passed = @($group.Group | Where-Object status -eq 'passed').Count; measuredSamples = $valid.Count; minimumThreeMeasuredSamples = ($valid.Count -ge 3)
            medianBytesPerSecond = $(if ($values.Count) { Get-Median $values } else { $null })
            minBytesPerSecond = $(if ($values.Count) { ($values | Measure-Object -Minimum).Minimum } else { $null })
            maxBytesPerSecond = $(if ($values.Count) { ($values | Measure-Object -Maximum).Maximum } else { $null })
            cpuMeasuredSamples = $cpuValid.Count
            gameThreadMeasuredSamples = $gameThreadValid.Count
            medianReplicateMeanMs = $(if ($cpuValid.Count) { Get-Median @($cpuValid | ForEach-Object { [double]$_.serverReplicateMeanMs }) } else { $null })
            medianReplicateP95Ms = $(if ($cpuValid.Count) { Get-Median @($cpuValid | ForEach-Object { [double]$_.serverReplicateP95Ms }) } else { $null })
            medianGameThreadMeanMs = $(if ($gameThreadValid.Count) { Get-Median @($gameThreadValid | ForEach-Object { [double]$_.gameThreadMeanMs }) } else { $null })
            medianGameThreadP95Ms = $(if ($gameThreadValid.Count) { Get-Median @($gameThreadValid | ForEach-Object { [double]$_.gameThreadP95Ms }) } else { $null })
            gameThreadAllZeroRuns = @($cpuValid | Where-Object { $_.gameThreadAllZero }).Count
            medianFullyDormantActorsMean = $(if ($dormantValid.Count) { Get-Median @($dormantValid | ForEach-Object { [double]$_.fullyDormantActorsMean }) } else { $null })
        }
    }
    $report.status = if ($PlanOnly) { 'planned' } elseif (@($report.cases | Where-Object { $_.status -ne 'passed' }).Count -eq 0 -and $report.cases.Count -eq $cases.Count) { 'passed' } else { 'failed' }
    Save-Report
    if (-not $PlanOnly -and @($report.cases | Where-Object { $_.mode -eq 'RideMotion' -and @($_.motionMetrics).Count -gt 0 }).Count -gt 0) {
        # 数据只取真实运行的 Client Jump；绘图失败不得仍把本轮标成完整通过。
        try {
            $plotScript = Join-Path $PSScriptRoot 'PlotRideMotionTimeline.py'
            $plotOutput = & python $plotScript (Join-Path $OutputDirectory 'report.json') --output-dir $OutputDirectory 2>&1
            if ($LASTEXITCODE -ne 0) { throw "Timeline plot failed: $plotOutput" }
            foreach ($line in $plotOutput) { Write-Output $line }
        } catch {
            $report.status = 'failed'
            foreach ($rideCase in @($report.cases | Where-Object mode -eq 'RideMotion')) {
                $rideCase.errors += "Timeline generation failed: $($_.Exception.Message)"
            }
            Save-Report
        }
    }
    if ($rows.Count) { $rows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'samples.csv') -NoTypeInformation -Encoding UTF8 }
    $markdown = New-Object 'System.Collections.Generic.List[string]'
    $markdown.Add('# Network validation report')
    $markdown.Add('')
    $markdown.Add("Run: $RunId; status: $($report.status); commit: $gitCommit; dirty: $($gitStatus.Count -gt 0).")
    $markdown.Add('')
    $markdown.Add('This is automated assertion evidence, not visual or manual end-to-end acceptance. Planned, failed and unsupported cases are not measured successes.')
    $markdown.Add('')
    $markdown.Add('| Case group | Passed / attempted | Measured samples | Median B/s | Min–max B/s | Replicate mean/P95 ms | GameThread mean/P95 ms | Dormant actors mean |')
    $markdown.Add('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
    foreach ($summary in $report.summaries) { $markdown.Add("| $($summary.group) | $($summary.passed) / $($summary.attempted) | $($summary.measuredSamples) | $($summary.medianBytesPerSecond) | $($summary.minBytesPerSecond) – $($summary.maxBytesPerSecond) | $($summary.medianReplicateMeanMs) / $($summary.medianReplicateP95Ms) | $($summary.medianGameThreadMeanMs) / $($summary.medianGameThreadP95Ms) | $($summary.medianFullyDormantActorsMean) |") }
    $markdown.Add('')
    $markdown.Add('CPU/actor columns summarize the median of per-run means/P95 values; P95 uses nearest-rank, not a pooled percentile. Missing metrics and all-zero GameThreadTime counters remain blank/null in the table and samples.csv; original counters remain in report.json. All-zero counters must not be interpreted as zero CPU cost. The process is capped at 60 fps: a 100 Hz replication setting is only an upper bound, not a measured sending rate.')
    foreach ($caseResult in $report.cases) { foreach ($warning in $caseResult.measurementWarnings) { $markdown.Add("Warning [$($caseResult.id)]: $warning") } }
    $markdown.Add('')
    $markdown.Add('See report.json for commands, configuration, fingerprints, per-assertion events and errors; samples.csv contains raw per-run samples. Bandwidth comparisons require matching topology, counts, warmup/sample durations and at least three successful measured samples per group. Visibility alone does not certify visual correctness.')
    $markdown.Add('RideMotion phase measurements are preserved as complete events in each case''s motionMetrics array in report.json, including role, phase, sample count, base/falling observations and correction distances. Missing or non-finite measurements fail the case; the numerical checks are not a replacement for visual acceptance.')
    if (@($report.cases | Where-Object mode -eq 'RideMotion').Count -gt 0 -and -not $PlanOnly) { $markdown.Add('RideMotion client Jump timeline: timeline-<case id>.svg, plus -samples.csv, -corrections.csv, -move-pairs.csv and -all-moves.csv. Moves are joined by CMC timestamp; server/client World clocks are not compared. This is not a packet trace or proof of visual smoothness.') }
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'summary.md'), ($markdown -join "`r`n"), $Utf8)
}
Write-Output "Report: $(Join-Path $OutputDirectory 'report.json')"
if ($PlanOnly) { Write-Output 'Plan only: no Unreal processes started and no measurements claimed.' }
elseif ($report.status -ne 'passed') { throw 'One or more network validation cases failed, were unsupported, or did not complete. See report.json.' }
