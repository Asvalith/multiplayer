[CmdletBinding()]
param(
    [ValidateSet('RideMotion', 'Combat', 'CombatTraffic', 'CombatReconnect', 'CombatLifecycle')][string]$Scenario = 'RideMotion',
    [ValidateSet('Normal', 'Moderate', 'Harsh')][string[]]$Profiles = @('Normal', 'Moderate', 'Harsh'),
    [ValidateRange(1, 100)][int]$Repeat = 3,
    [ValidateRange(30, 1800)][int]$TimeoutSeconds = 180,
    [ValidateRange(1, 60)][int]$PlatformNetHz = 30,
    [ValidateSet('OrderedVelocity', 'PlatformInertia')][string]$PlatformSyncMode = 'PlatformInertia',
    [ValidateSet('Moving', 'Static')][string]$PlatformMotion = 'Moving',
    [string]$EditorPath = 'E:/program/ue554/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe',
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
    # UMG 布局、按钮图、菜单控制器和 HUD 生命周期均在 Content/UI，统一纳入验证指纹。
    foreach ($directory in @('Source', 'Config', 'Scripts', 'Tests', 'Content/UI', 'Content/Combat', 'Content/__ExternalActors__/Combat')) {
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
    param([string]$Mode = 'RideMotion')
    if ($Mode -eq 'Combat') {
        $required = @('DedicatedAuthority', 'PartnerReady', 'Join', 'ServerCombatFixture', 'ServerCombatSpawn',
            'ServerCombatCurrentState', 'ServerCombatAttack', 'ServerCombatDefense', 'ServerCombatHeal',
            'ServerCombatEnemyDeath', 'ServerCombatPlayerDeath', 'PrimaryCurrentState', 'PartnerCurrentState',
            'PrimaryAttack', 'PartnerAttack', 'PrimaryDefense', 'PartnerDefense', 'PrimaryHeal', 'PartnerHeal',
            'PrimaryDefensePredicted', 'PrimaryDefenseRepeatedInput', 'PrimaryDefenseConfirmed',
            'PrimaryAttackPredicted', 'PrimaryAttackRepeatedInput', 'PrimaryAttackConfirmed',
            'PrimaryHealPredicted', 'PrimaryHealRepeatedInput', 'PrimaryHealConfirmed',
            'ServerCombatResourceFixture', 'ServerCombatResourceRejected', 'PrimaryResourceReady', 'PartnerResourceReady',
            'PrimaryResourcePredicted', 'PrimaryResourceRepeatedInput', 'PrimaryResourceRejected', 'PrimaryResourceRollback', 'PartnerResourceRollback',
            'PrimaryResourceLocalRollback',
            'ServerCombatDeathFixture', 'PrimaryDeathReady', 'PartnerDeathReady', 'PrimaryDeathPredicted', 'PrimaryDeathRepeatedInput', 'PrimaryDeathRejected',
            'PrimaryDeathAttackPredicted', 'PrimaryDeathAttackRepeatedInput', 'PrimaryDeathAttackRejected',
            'PrimaryDeathHealPredicted', 'PrimaryDeathHealRepeatedInput', 'PrimaryDeathHealRejected', 'PrimaryDeathLocalRollback',
            'PrimaryFreshIntentionalExit', 'ServerCombatFreshSession',
            'ServerCombatResourceRoutingFixture', 'ServerCombatResourceRouting', 'PartnerDefenseResourcesHidden', 'PartnerResourceRollbackResourcesHidden',
            'PrimaryPublicMaxHealth', 'PartnerPublicMaxHealth', 'PartnerOwnedResources', 'PrimaryPrivateResourcesHidden',
            'PrimaryEnemyDeath', 'PartnerEnemyDeath', 'PrimaryPlayerDeath', 'PartnerPlayerDeath', 'Complete', 'PartnerComplete')
        foreach ($case in @('AttackCooldown', 'DefenseCooldown', 'HealCooldown', 'HealEnergy', 'AreaCooldown', 'AreaEnergy', 'HealNoTarget')) {
            $required += @("Primary${case}Ready", "Partner${case}Ready", "Server${case}Fixture",
                "Primary${case}Predicted", "Primary${case}RepeatedInput", "Primary${case}Rejected", "Primary${case}Rollback", "Server${case}Rejected",
                "Primary${case}RetryReady", "Partner${case}RetryReady", "Primary${case}RetryPredicted", "Primary${case}RetryRepeatedInput",
                "Primary${case}RetryConfirmed", "Primary${case}RetryOutcome", "Partner${case}RetryOutcome", "Server${case}Retry")
        }
        foreach ($case in @('FreshAttack', 'FreshDefense', 'FreshHeal')) {
            $required += @("Primary${case}Ready", "Partner${case}Ready", "Primary${case}RetryPredicted", "Primary${case}RetryRepeatedInput",
                "Primary${case}RetryConfirmed", "Primary${case}RetryOutcome", "Partner${case}RetryOutcome", "Server${case}Retry")
        }
        return $required
    }
    if ($Mode -eq 'CombatTraffic') {
        return @('DedicatedAuthority', 'PartnerReady', 'Join', 'ServerTrafficFixture', 'ServerTrafficClearPath', 'ServerTrafficConnections',
            'ServerTrafficSchedule', 'PrimaryTrafficReady', 'PartnerTrafficReady', 'PrimaryTrafficObserved', 'PartnerTrafficObserved', 'Complete', 'PartnerComplete')
    }
    if ($Mode -eq 'CombatLifecycle') {
        $required = @('DedicatedAuthority', 'PartnerReady', 'Join', 'ServerLifecycleFloor', 'ServerLifecycleLateState',
            'ServerLifecycleFixture', 'ServerLifecycleRules', 'PrimaryLifecycleRules', 'PartnerLifecycleRules',
            'PrimaryLifecycleLateState', 'PartnerLifecycleLateState',
            'ServerAreaSingleDamage', 'ServerAreaCancelNoDamage', 'ServerAreaStunInterrupt', 'ServerAreaStunNoDamage',
            'ServerAreaCancelCooldownRetained', 'ServerAreaConfirmedCancelCooldownRetained', 'ServerAreaConfirmedCancelNoDamage',
            'PrimaryAreaConfirmedCancelAuthorityObserved', 'PrimaryAreaConfirmedCancelSent',
            'PrimaryAreaCancelSent', 'PrimaryAreaStunObserved', 'PartnerAreaStunObserved', 'Complete', 'PartnerComplete')
        foreach ($case in @('Release', 'Cancel', 'ConfirmedCancel', 'Stun')) {
            $required += @("PrimaryArea${case}Ready", "PartnerArea${case}Ready", "PrimaryArea${case}Predicted",
                "PrimaryArea${case}RepeatedInput", "PrimaryArea${case}Outcome", "PartnerArea${case}Outcome")
        }
        foreach ($case in @('Join', 'Release', 'Cancel', 'ConfirmedCancel', 'Stun')) {
            $required += @("PrimaryMovement${case}Ready", "PartnerMovement${case}Ready",
                "PrimaryMovement${case}", "PartnerMovement${case}", "ServerMovement${case}")
        }
        return $required
    }
    if ($Mode -eq 'CombatReconnect') {
        return @('DedicatedAuthority', 'PartnerReady', 'Join', 'ServerReconnectFixture',
            'ServerReconnectTicketActive', 'ServerReconnectTicketInvalid', 'ServerReconnectTicketAckLost', 'ServerReconnectTicketRotatedAlias',
            'ServerReconnectLive', 'ServerReconnectCooldownElapsed', 'ServerReconnectControlElapsed', 'ServerReconnectDead', 'ServerReconnectPartnerContinuity', 'ServerReconnectSharedState',
            'PrimaryReconnectLiveBefore', 'PrimaryReconnectLiveDrop', 'PrimaryReconnectLive',
            'PrimaryReconnectDeadBefore', 'PrimaryReconnectDeadDrop', 'PrimaryReconnectDead',
            'PartnerReconnectLiveBefore', 'PartnerReconnectLive', 'PartnerReconnectDeadBefore', 'PartnerReconnectDead',
            'PrimaryReconnectControl', 'PartnerReconnectControl', 'Complete', 'PartnerComplete')
    }
    return @('DedicatedAuthority', 'PartnerReady', 'Join', 'MotionStand', 'MotionWalk', 'MotionJump', 'MotionReverse',
        'ServerMotionStand', 'ServerMotionWalk', 'ServerMotionReverse', 'ServerMotionJump', 'MotionServerObserved', 'PartnerComplete')
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
    $successfulAssertions = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::Ordinal)
    $unsupported = @($Events | Where-Object { $_.status -eq 'UNSUPPORTED' -or (Get-PropertyValue $_ 'phase' '') -eq 'Unsupported' }).Count -gt 0
    foreach ($event in $Events) {
        if ($event.status -eq 'FAIL' -or ($event.status -eq 'ASSERT' -and (Get-PropertyValue $event 'passed' $false) -cne $true)) {
            $errors.Add(('Driver failure: {0} {1}' -f (Get-PropertyValue $event 'assertion' ''), (Get-PropertyValue $event 'detail' '')))
        }
        elseif ($event.status -eq 'ASSERT') {
            $assertion = [string](Get-PropertyValue $event 'assertion' '')
            [void]$successfulAssertions.Add("$(Get-PropertyValue $event 'role' '')|$($assertion.ToLowerInvariant())")
        }
    }
    foreach ($role in @('Server', 'Client', 'Partner')) {
        if (-not (Test-RoleDone -Events $Events -Role $role)) { $errors.Add("Missing successful DONE from $role") }
    }
    foreach ($name in $Required) {
        # 服务器自身通过不能代替远端确认；两端入场也必须分别有证据。
        $roles = if ($name -eq 'Join') { @('Server', 'Client') }
            elseif ($name -like 'Partner*') { @('Partner') }
            elseif ($name -like 'Primary*' -or ($Mode -in @('Combat', 'CombatTraffic', 'CombatReconnect', 'CombatLifecycle') -and $name -eq 'Complete')) { @('Client') }
            elseif ($name -like 'Motion*' -and $name -ne 'MotionServerObserved') { @('Client') }
            else { @('Server') }
        foreach ($role in $roles) {
            if (-not $successfulAssertions.Contains("$role|$($name.ToLowerInvariant())")) { $errors.Add("Missing passing assertion: $name from $role") }
        }
    }
    # 运动样本必须按角色与阶段保留，不能被最后一条服务器样本覆盖，也不能用规模测试指标代替。
    $motionMetrics = @($Events | Where-Object { $_.status -eq 'METRIC' -and (Get-PropertyValue (Get-PropertyValue $_ 'metrics') 'category' '') -ceq 'platformRide' })
    $combatTrafficMetrics = @($Events | Where-Object { $_.status -eq 'METRIC' -and (Get-PropertyValue (Get-PropertyValue $_ 'metrics') 'category' '') -ceq 'combatTraffic' })
    $metrics = $null
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
    if ($Mode -eq 'CombatTraffic') {
        if ($combatTrafficMetrics.Count -ne 1) { $errors.Add('CombatTraffic requires exactly one combatTraffic metric, from Server') }
        foreach ($sample in $combatTrafficMetrics) {
            $traffic = $sample.metrics
            if ((Get-PropertyValue $sample 'role' '') -cne 'Server' -or (Get-PropertyValue $sample 'phase' '') -cne 'TrafficSampling' -or
                (Get-PropertyValue $traffic 'counterSource' '') -cne 'UNetDriver.OutTotalBytes/OutTotalPackets' -or
                (Get-PropertyValue $traffic 'scope' '') -cne 'server-all-client-connections' -or
                (Get-PropertyValue $traffic 'scheduleVersion' '') -cne 'projectile-energy-v2') { $errors.Add('CombatTraffic metric has the wrong role, phase, scope, counter source or schedule') }
            if ((Get-PropertyValue $traffic 'trajectoryMode' '') -ceq 'initialLaunchState') {
                $replicateMovement = Get-PropertyValue $traffic 'replicateMovement'
                if ($replicateMovement -isnot [bool] -or $replicateMovement) { $errors.Add('Initial-launch trajectory unexpectedly replicated continuous movement') }
                foreach ($peer in @('Client', 'Partner')) {
                    $assertion = if ($peer -eq 'Client') { 'PrimaryTrafficLaunchState' } else { 'PartnerTrafficLaunchState' }
                    if (@($Events | Where-Object { $_.status -eq 'ASSERT' -and (Get-PropertyValue $_ 'role' '') -ceq $peer -and
                        (Get-PropertyValue $_ 'phase' '') -ceq 'TrafficVerify' -and (Get-PropertyValue $_ 'assertion' '') -ceq $assertion -and
                        (Get-PropertyValue $_ 'passed' $false) -ceq $true }).Count -eq 0) {
                        $errors.Add("Missing passing initial-launch observation: $peer")
                    }
                }
            }
            elseif ((Get-PropertyValue $traffic 'trajectoryMode' '') -cne '') { $errors.Add('Unknown CombatTraffic trajectory contract') }
            $validNumbers = $true
            foreach ($field in @('warmupSeconds', 'targetSampleSeconds', 'elapsedSeconds', 'beginOutTotalBytes', 'endOutTotalBytes', 'outBytes',
                'beginOutTotalPackets', 'endOutTotalPackets', 'outPackets', 'bytesPerSecond', 'packetsPerSecond', 'projectileSpawns', 'energyUpdates',
                'connectionCount', 'projectileIntervalSeconds', 'energyIntervalSeconds', 'maxScheduleDelaySeconds', 'projectileNetHz', 'frameSamples', 'framesPerSecond', 'serverNetTickRate', 'maxFps')) {
                $value = Get-PropertyValue $traffic $field
                if (-not (Test-FiniteNumber $value)) { $errors.Add("Invalid combatTraffic metric: $field must be finite"); $validNumbers = $false }
                elseif ([double]$value -lt 0) { $errors.Add("Invalid combatTraffic metric: $field is negative"); $validNumbers = $false }
            }
            if (-not $validNumbers) { continue }
            foreach ($field in @('beginOutTotalBytes', 'endOutTotalBytes', 'outBytes', 'beginOutTotalPackets', 'endOutTotalPackets', 'outPackets', 'projectileSpawns', 'energyUpdates', 'connectionCount', 'frameSamples')) {
                if ([double]$traffic.$field -ne [Math]::Truncate([double]$traffic.$field)) { $errors.Add("Invalid combatTraffic integer: $field") }
            }
            foreach ($field in @('beginOutTotalBytes', 'endOutTotalBytes', 'beginOutTotalPackets', 'endOutTotalPackets')) {
                if ([double]$traffic.$field -gt [uint32]::MaxValue) { $errors.Add("Invalid combatTraffic uint32 counter: $field") }
            }
            if ($traffic.warmupSeconds -lt 3 -or $traffic.warmupSeconds -gt 3.25 -or $traffic.targetSampleSeconds -ne 15 -or
                $traffic.elapsedSeconds -lt 15 -or $traffic.elapsedSeconds -gt 15.25 -or $traffic.maxScheduleDelaySeconds -ge .25 -or
                $traffic.projectileIntervalSeconds -ne .5 -or $traffic.energyIntervalSeconds -ne .25 -or
                $traffic.projectileSpawns -ne 30 -or $traffic.energyUpdates -ne 60 -or $traffic.connectionCount -ne 2 -or $traffic.maxFps -ne 30) {
                $errors.Add('CombatTraffic fixed workload, timing or connection contract was not met')
            }
            if ($traffic.framesPerSecond -lt 28 -or $traffic.framesPerSecond -gt 34) { $errors.Add('CombatTraffic measured server frames/second must be within 28..34 for comparable samples') }
            if ($traffic.outBytes -le 0 -or $traffic.outPackets -le 0 -or $traffic.frameSamples -le 0 -or $traffic.projectileNetHz -le 0 -or $traffic.framesPerSecond -le 0 -or
                $traffic.endOutTotalBytes -le $traffic.beginOutTotalBytes -or $traffic.endOutTotalPackets -le $traffic.beginOutTotalPackets -or
                $traffic.outBytes -ne ($traffic.endOutTotalBytes - $traffic.beginOutTotalBytes) -or $traffic.outPackets -ne ($traffic.endOutTotalPackets - $traffic.beginOutTotalPackets)) {
                $errors.Add('CombatTraffic requires positive traffic and frame samples, unchanged counter lifetime, and exact counter deltas')
            }
            if ($traffic.elapsedSeconds -gt 0) {
                foreach ($pair in @(@('bytesPerSecond', 'outBytes'), @('packetsPerSecond', 'outPackets'), @('framesPerSecond', 'frameSamples'))) {
                    $expectedRate = [double]$traffic.($pair[1]) / [double]$traffic.elapsedSeconds
                    if ([Math]::Abs([double]$traffic.($pair[0]) - $expectedRate) -gt [Math]::Max(0.000001, $expectedRate * 0.000001)) { $errors.Add("CombatTraffic rate does not match measured elapsed time: $($pair[0])") }
                }
            }
        }
    }
    $status = 'passed'
    if ($errors.Count -gt 0) { $status = 'failed' }
    if ($unsupported) { $status = 'unsupported' }
    return [pscustomobject]@{ status = $status; errors = @($errors.ToArray()); metrics = $metrics; motionMetrics = $motionMetrics; combatTrafficMetrics = $combatTrafficMetrics }
}

if ($SelfTest) {
    foreach ($excluded in @('Source/.vs/index.db', 'Source\.git\objects\cache', 'Source/Binaries/game.exe', 'Tests/Intermediate/test.obj', 'Scripts/Saved/log.txt', 'Tests/Evidence/report.json', 'Source/Nested/.VS/index.db')) {
        if (Test-SourceManifestPath $excluded) { throw "Source manifest accepted generated/cache path: $excluded" }
    }
    foreach ($included in @('Source/multiplayer/Core/GameMode.cpp', 'Source/multiplayer/Save/SavedState.h', 'Config/DefaultGame.ini', 'Scripts/RunMultiplayerNetworkTests.ps1', 'Tests/EvidenceReader.cpp', 'Content/Config/Gameplay.json')) {
        if (-not (Test-SourceManifestPath $included)) { throw "Source manifest excluded source/config path: $included" }
    }
    # 构造完整的双端四阶段日志，再逐项破坏证据，防止“只有 DONE”或服务器代答造成假通过。
    $motionLines = @()
    foreach ($role in @('Server', 'Client')) {
        $assertions = if ($role -eq 'Server') { @('DedicatedAuthority', 'Join', 'ServerMotionStand', 'ServerMotionWalk', 'ServerMotionReverse', 'ServerMotionJump', 'MotionServerObserved') } else { @('Join', 'MotionStand', 'MotionWalk', 'MotionJump', 'MotionReverse') }
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
    foreach ($assertion in @('PartnerReady', 'PartnerComplete')) {
        $motionLines += 'Log: COOP_TEST ' + (@{ token = 'motion'; mode = 'RideMotion'; role = 'Partner'; status = 'ASSERT'; assertion = $assertion; passed = $true } | ConvertTo-Json -Compress)
    }
    $motionLines += 'Log: COOP_TEST {"token":"motion","mode":"RideMotion","role":"Partner","status":"DONE","passed":true}'
    $motionFixture = $motionLines -join "`n"
    $motionEvents = @(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Server' -Mode 'RideMotion') +
        @(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Client' -Mode 'RideMotion') +
        @(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Partner' -Mode 'RideMotion')
    $motionRequired = @(Get-RequiredAssertions)
    $motionVerdict = Get-CaseVerdict $motionEvents $motionRequired 'RideMotion'
    if ($motionVerdict.status -ne 'passed' -or $motionVerdict.motionMetrics.Count -ne 8 -or $null -ne $motionVerdict.metrics) { throw 'Runner did not preserve valid per-role motion measurements' }
    if (@(ConvertFrom-CoopLog $motionFixture -Token 'old' -Role 'Server' -Mode 'RideMotion').Count) { throw 'Runner accepted events from another run' }
    if (@(ConvertFrom-CoopLog $motionFixture -Token 'motion' -Role 'Server' -Mode 'Removed').Count) { throw 'Runner accepted events from another scenario' }
    foreach ($role in @('Server', 'Client', 'Partner')) {
        if ((Get-CaseVerdict @($motionEvents | Where-Object role -ne $role) $motionRequired 'RideMotion').status -ne 'failed') { throw "Runner accepted missing peer: $role" }
    }
    foreach ($assertion in $motionRequired) {
        $missing = @($motionEvents | Where-Object { $_.status -ne 'ASSERT' -or $_.assertion -ne $assertion })
        if ((Get-CaseVerdict $missing $motionRequired 'RideMotion').status -ne 'failed') { throw "Runner accepted missing assertion: $assertion" }
    }
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

    # 战斗模式不要求运动样本，但每个权威检查和两条连接的独立观察都不能缺失。
    $combatRequired = @(Get-RequiredAssertions -Mode 'Combat')
    $combatEvents = @()
    foreach ($name in $combatRequired) {
        $roles = if ($name -eq 'Join') { @('Server', 'Client') }
            elseif ($name -like 'Partner*') { @('Partner') }
            elseif ($name -like 'Primary*' -or $name -eq 'Complete') { @('Client') }
            else { @('Server') }
        foreach ($role in $roles) {
            $combatEvents += [pscustomobject]@{ token = 'combat'; mode = 'Combat'; role = $role; status = 'ASSERT'; assertion = $name; passed = $true }
        }
    }
    foreach ($role in @('Server', 'Client', 'Partner')) {
        $combatEvents += [pscustomobject]@{ token = 'combat'; mode = 'Combat'; role = $role; status = 'DONE'; passed = $true }
    }
    if ((Get-CaseVerdict $combatEvents $combatRequired 'Combat').status -ne 'passed') { throw 'Runner rejected complete independent combat evidence' }
    foreach ($role in @('Server', 'Client', 'Partner')) {
        if ((Get-CaseVerdict @($combatEvents | Where-Object role -ne $role) $combatRequired 'Combat').status -ne 'failed') { throw "Combat runner accepted missing peer: $role" }
    }
    foreach ($name in $combatRequired) {
        $missing = @($combatEvents | Where-Object { $_.status -ne 'ASSERT' -or $_.assertion -ne $name })
        if ((Get-CaseVerdict $missing $combatRequired 'Combat').status -ne 'failed') { throw "Combat runner accepted missing assertion: $name" }
    }
    foreach ($role in @('Client', 'Partner')) {
        $wrongRole = $combatEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        foreach ($event in $wrongRole) {
            if ($event.status -eq 'ASSERT' -and $event.role -eq $role) { $event.role = 'Server' }
        }
        if ((Get-CaseVerdict $wrongRole $combatRequired 'Combat').status -ne 'failed') { throw "Server incorrectly satisfied combat client assertions: $role" }
    }
    $reconnectRequired = @(Get-RequiredAssertions -Mode 'CombatReconnect')
    $reconnectEvents = @()
    foreach ($name in $reconnectRequired) {
        $roles = if ($name -eq 'Join') { @('Server', 'Client') } elseif ($name -like 'Partner*') { @('Partner') }
            elseif ($name -like 'Primary*' -or $name -eq 'Complete') { @('Client') } else { @('Server') }
        foreach ($role in $roles) { $reconnectEvents += [pscustomobject]@{ token = 'reconnect'; mode = 'CombatReconnect'; role = $role; status = 'ASSERT'; assertion = $name; passed = $true } }
    }
    foreach ($role in @('Server', 'Client', 'Partner')) { $reconnectEvents += [pscustomobject]@{ token = 'reconnect'; mode = 'CombatReconnect'; role = $role; status = 'DONE'; passed = $true } }
    if ((Get-CaseVerdict $reconnectEvents $reconnectRequired 'CombatReconnect').status -ne 'passed') { throw 'Runner rejected complete reconnect evidence' }
    foreach ($name in $reconnectRequired) {
        $missing = @($reconnectEvents | Where-Object { $_.status -ne 'ASSERT' -or $_.assertion -ne $name })
        if ((Get-CaseVerdict $missing $reconnectRequired 'CombatReconnect').status -ne 'failed') { throw "CombatReconnect accepted missing assertion: $name" }
    }
    foreach ($role in @('Client', 'Partner')) {
        $wrongRole = $reconnectEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        foreach ($event in $wrongRole) { if ($event.status -eq 'ASSERT' -and $event.role -eq $role) { $event.role = 'Server' } }
        if ((Get-CaseVerdict $wrongRole $reconnectRequired 'CombatReconnect').status -ne 'failed') { throw "Server incorrectly satisfied reconnect client assertions: $role" }
    }
    # 新施法模式仍要求三进程证据；缺一条或用服务器替代客户端都必须失败。
    $lifecycleRequired = @(Get-RequiredAssertions -Mode 'CombatLifecycle')
    $lifecycleEvents = @()
    foreach ($name in $lifecycleRequired) {
        $roles = if ($name -eq 'Join') { @('Server', 'Client') } elseif ($name -like 'Partner*') { @('Partner') }
            elseif ($name -like 'Primary*' -or $name -eq 'Complete') { @('Client') } else { @('Server') }
        foreach ($role in $roles) { $lifecycleEvents += [pscustomobject]@{ token = 'lifecycle'; mode = 'CombatLifecycle'; role = $role; status = 'ASSERT'; assertion = $name; passed = $true } }
    }
    foreach ($role in @('Server', 'Client', 'Partner')) { $lifecycleEvents += [pscustomobject]@{ token = 'lifecycle'; mode = 'CombatLifecycle'; role = $role; status = 'DONE'; passed = $true } }
    if ((Get-CaseVerdict $lifecycleEvents $lifecycleRequired 'CombatLifecycle').status -ne 'passed') { throw 'Runner rejected complete lifecycle evidence' }
    foreach ($name in $lifecycleRequired) {
        $missing = @($lifecycleEvents | Where-Object { $_.status -ne 'ASSERT' -or $_.assertion -ne $name })
        if ((Get-CaseVerdict $missing $lifecycleRequired 'CombatLifecycle').status -ne 'failed') { throw "CombatLifecycle accepted missing assertion: $name" }
    }
    foreach ($role in @('Client', 'Partner')) {
        $wrongRole = $lifecycleEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        foreach ($event in $wrongRole) { if ($event.status -eq 'ASSERT' -and $event.role -eq $role) { $event.role = 'Server' } }
        if ((Get-CaseVerdict $wrongRole $lifecycleRequired 'CombatLifecycle').status -ne 'failed') { throw "Server incorrectly satisfied lifecycle client assertions: $role" }
    }
    $trafficRequired = @(Get-RequiredAssertions -Mode 'CombatTraffic')
    $trafficEvents = @()
    foreach ($name in $trafficRequired) {
        $roles = if ($name -eq 'Join') { @('Server', 'Client') } elseif ($name -like 'Partner*') { @('Partner') }
            elseif ($name -like 'Primary*' -or $name -eq 'Complete') { @('Client') } else { @('Server') }
        foreach ($role in $roles) { $trafficEvents += [pscustomobject]@{ token = 'traffic'; mode = 'CombatTraffic'; role = $role; status = 'ASSERT'; assertion = $name; passed = $true } }
    }
    foreach ($role in @('Server', 'Client', 'Partner')) { $trafficEvents += [pscustomobject]@{ token = 'traffic'; mode = 'CombatTraffic'; role = $role; status = 'DONE'; passed = $true } }
    $trafficMetric = [pscustomobject]@{ token = 'traffic'; mode = 'CombatTraffic'; role = 'Server'; phase = 'TrafficSampling'; status = 'METRIC'; metrics = [pscustomobject]@{
        category = 'combatTraffic'; counterSource = 'UNetDriver.OutTotalBytes/OutTotalPackets'; scope = 'server-all-client-connections'; scheduleVersion = 'projectile-energy-v2'
        warmupSeconds = 3.1; targetSampleSeconds = 15; elapsedSeconds = 15; beginOutTotalBytes = 10000; endOutTotalBytes = 22000; outBytes = 12000
        beginOutTotalPackets = 50; endOutTotalPackets = 350; outPackets = 300; bytesPerSecond = 800; packetsPerSecond = 20
        projectileSpawns = 30; energyUpdates = 60; connectionCount = 2; projectileIntervalSeconds = .5; energyIntervalSeconds = .25; maxScheduleDelaySeconds = .1
        projectileNetHz = 60; frameSamples = 450; framesPerSecond = 30; serverNetTickRate = 30; maxFps = 30
    } }
    $trafficEvents += $trafficMetric
    $trafficVerdict = Get-CaseVerdict $trafficEvents $trafficRequired 'CombatTraffic'
    if ($trafficVerdict.status -ne 'passed' -or $trafficVerdict.combatTrafficMetrics.Count -ne 1) { throw "Runner rejected valid combatTraffic evidence: $($trafficVerdict.errors -join '; ')" }
    foreach ($role in @('Server', 'Client', 'Partner')) {
        if ((Get-CaseVerdict @($trafficEvents | Where-Object role -ne $role) $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "CombatTraffic accepted missing peer: $role" }
    }
    foreach ($name in $trafficRequired) {
        $missing = @($trafficEvents | Where-Object { $_.status -ne 'ASSERT' -or $_.assertion -ne $name })
        if ((Get-CaseVerdict $missing $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "CombatTraffic accepted missing assertion: $name" }
    }
    if ((Get-CaseVerdict @($trafficEvents | Where-Object status -ne 'METRIC') $trafficRequired 'CombatTraffic').status -ne 'failed') { throw 'CombatTraffic accepted missing traffic measurement' }
    if ((Get-CaseVerdict @($trafficEvents + $trafficMetric) $trafficRequired 'CombatTraffic').status -ne 'failed') { throw 'CombatTraffic accepted duplicate traffic measurements' }
    foreach ($invalidCase in @(
        @{ field = 'category'; value = 'scale' }, @{ field = 'counterSource'; value = 'log-line-count' }, @{ field = 'scope'; value = 'GAS-only' },
        @{ field = 'scheduleVersion'; value = 'projectile-energy-v1' }, @{ field = 'warmupSeconds'; value = 2.99 }, @{ field = 'warmupSeconds'; value = 4 },
        @{ field = 'targetSampleSeconds'; value = 20 }, @{ field = 'elapsedSeconds'; value = 0 }, @{ field = 'elapsedSeconds'; value = 15.26 },
        @{ field = 'outBytes'; value = $null }, @{ field = 'outPackets'; value = -1 }, @{ field = 'outPackets'; value = 1.5 }, @{ field = 'outPackets'; value = 0 },
        @{ field = 'endOutTotalBytes'; value = 5000 }, @{ field = 'endOutTotalBytes'; value = 4294967296 }, @{ field = 'beginOutTotalBytes'; value = 'NaN' },
        @{ field = 'endOutTotalPackets'; value = 349 }, @{ field = 'bytesPerSecond'; value = 'Infinity' }, @{ field = 'packetsPerSecond'; value = 21 },
        @{ field = 'projectileSpawns'; value = 29 }, @{ field = 'energyUpdates'; value = 59 }, @{ field = 'connectionCount'; value = 3 },
        @{ field = 'projectileIntervalSeconds'; value = .6 }, @{ field = 'energyIntervalSeconds'; value = .2 }, @{ field = 'maxScheduleDelaySeconds'; value = .25 },
        @{ field = 'projectileNetHz'; value = 0 }, @{ field = 'frameSamples'; value = 0 }, @{ field = 'framesPerSecond'; value = 60 },
        @{ field = 'serverNetTickRate'; value = -1 }, @{ field = 'maxFps'; value = 60 }
    )) {
        $invalidEvents = $trafficEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        $sample = @($invalidEvents | Where-Object status -eq 'METRIC')[0]
        $sample.metrics.($invalidCase.field) = $invalidCase.value
        if ((Get-CaseVerdict $invalidEvents $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "CombatTraffic accepted invalid field: $($invalidCase.field)" }
    }
    foreach ($field in @('role', 'phase')) {
        $invalidEvents = $trafficEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        @($invalidEvents | Where-Object status -eq 'METRIC')[0].$field = 'Wrong'
        if ((Get-CaseVerdict $invalidEvents $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "CombatTraffic accepted wrong metric $field" }
    }
    # 同步改真实帧数和计算帧率，确保帧率边界本身受测，而不是被速率不一致检查碰巧拦截。
    foreach ($fps in @(27, 28, 34, 35)) {
        $fpsEvents = $trafficEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        $sample = @($fpsEvents | Where-Object status -eq 'METRIC')[0]
        $sample.metrics.frameSamples = $fps * $sample.metrics.elapsedSeconds
        $sample.metrics.framesPerSecond = $fps
        $expected = if ($fps -in @(28, 34)) { 'passed' } else { 'failed' }
        if ((Get-CaseVerdict $fpsEvents $trafficRequired 'CombatTraffic').status -ne $expected) { throw "CombatTraffic actual FPS boundary failed: $fps" }
    }
    foreach ($role in @('Client', 'Partner')) {
        $wrongRole = $trafficEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        foreach ($event in $wrongRole) { if ($event.status -eq 'ASSERT' -and $event.role -eq $role) { $event.role = 'Server' } }
        if ((Get-CaseVerdict $wrongRole $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "Server incorrectly satisfied CombatTraffic client assertions: $role" }
    }
    # 新版必须用实际两端的发射快照观察，且额外测试回执不得混入采样窗口；旧版基线仍可读。
    $launchEvents = $trafficEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
    $launchMetric = @($launchEvents | Where-Object status -eq 'METRIC')[0].metrics
    $launchMetric | Add-Member -NotePropertyName trajectoryMode -NotePropertyValue 'initialLaunchState'
    $launchMetric | Add-Member -NotePropertyName replicateMovement -NotePropertyValue $false
    foreach ($peer in @('Client', 'Partner')) {
        $name = if ($peer -eq 'Client') { 'PrimaryTrafficLaunchState' } else { 'PartnerTrafficLaunchState' }
        $launchEvents += [pscustomobject]@{ token = 'traffic'; mode = 'CombatTraffic'; role = $peer; phase = 'TrafficVerify'; status = 'ASSERT'; assertion = $name; passed = $true }
    }
    if ((Get-CaseVerdict $launchEvents $trafficRequired 'CombatTraffic').status -ne 'passed') { throw 'Runner rejected valid initial-launch evidence' }
    foreach ($value in @($null, $true, 'false', 0)) {
        $invalidEvents = $launchEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        @($invalidEvents | Where-Object status -eq 'METRIC')[0].metrics.replicateMovement = $value
        if ((Get-CaseVerdict $invalidEvents $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "Initial-launch accepted invalid replicateMovement: $value" }
    }
    foreach ($peer in @('Client', 'Partner')) {
        $missing = @($launchEvents | Where-Object { $_.status -ne 'ASSERT' -or $_.role -ne $peer -or $_.assertion -notlike '*TrafficLaunchState' })
        if ((Get-CaseVerdict $missing $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "Initial-launch accepted missing observer: $peer" }
        $duringSample = $launchEvents | ConvertTo-Json -Depth 6 | ConvertFrom-Json
        foreach ($event in $duringSample) { if ($event.status -eq 'ASSERT' -and $event.role -eq $peer -and $event.assertion -like '*TrafficLaunchState') { $event.phase = 'TrafficSampling' } }
        if ((Get-CaseVerdict $duringSample $trafficRequired 'CombatTraffic').status -ne 'failed') { throw "Initial-launch observation polluted measurement window: $peer" }
    }
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
    environment = [ordered]@{ machine = $env:COMPUTERNAME; engineAssociation = $projectDescription.EngineAssociation; executable = $ExecutablePath; executableVersion = $exeVersion; executableSha256 = $exeHash; projectModule = $projectModule; runtimeGameplayConfig = $runtimeConfig; buildKind = $BuildKind; map = $Map; nullRhi = (-not ($Visible -or $Offscreen)); offscreenDx12 = [bool]$Offscreen; topology = 'One local dedicated server and two separate remote client processes'; visualValidation = $false }
    provenance = [ordered]@{ gitCommit = $gitCommit; dirty = ($gitStatus.Count -gt 0); gitStatus = $gitStatus; sourceFingerprintSha256 = $fingerprint; sourceManifest = $sourceManifest; gameplayConfigPath = $configPath; gameplayConfigSha256 = $configHash; gameplayConfigText = $configText }
    scope = @('Assertions represent the driver checks, not manual visual acceptance.', 'PktLag values are emulator parameters applied to all three peers, not measured RTT.', 'RideMotion retains per-role, per-phase runtime measurements; they do not certify rendered smoothness.', 'CombatTraffic uses server UNetDriver cumulative outbound bytes/packets, including overhead, ACKs, retransmissions and all game/test traffic. Emulated loss/delay can still be counted; these are not NIC packet captures or GAS-only costs.', 'Provenance gameplayConfig fields describe source files; environment.runtimeGameplayConfig describes the located runtime file, not proof that the engine loaded it.')
    cases = @(); summaries = @()
}
$cases = New-Object 'System.Collections.Generic.List[object]'
$profileSettings = @{
    Normal = @{ lag = 0; variance = 0; loss = 0 }
    Moderate = @{ lag = 100; variance = 20; loss = 2 }
    Harsh = @{ lag = 200; variance = 50; loss = 5 }
}
foreach ($profile in $Profiles) { for ($iteration = 1; $iteration -le $Repeat; $iteration++) {
    $cases.Add([pscustomobject]@{ mode = $Scenario; profile = $profile; iteration = $iteration })
} }

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
        $caseId = '{0:D3}-{1}-{2}-{3}' -f $caseIndex, $case.mode, $case.profile, $case.iteration
        $port = if ($ListenPort -gt 0) { $ListenPort } else { Get-FreeUdpPort }
        $token = "$RunId-$caseIndex"
        $result = [ordered]@{
            id = $caseId; token = $token; mode = $case.mode; profile = $case.profile; iteration = $case.iteration
            platformNetHz = $PlatformNetHz; platformSyncMode = $PlatformSyncMode; platformMotion = $PlatformMotion
            samplingDurationSource = $(if ($case.mode -eq 'CombatTraffic') { 'combatTrafficMetrics[0].metrics.elapsedSeconds; server monotonic clock, excludes setup/warmup/verification' } else { 'motionMetrics[].metrics.elapsedSeconds, separately measured per role and phase' })
            combatTrafficContract = $(if ($case.mode -eq 'CombatTraffic') { [ordered]@{ scheduleVersion = 'projectile-energy-v2'; warmupSeconds = 3; sampleSeconds = 15; projectileIntervalSeconds = .5; projectileSpawns = 30; energyIntervalSeconds = .25; energyUpdates = 60; energyValues = @(30, 70); maxFps = 30; minMeasuredServerFps = 28; maxMeasuredServerFps = 34; connections = 2 } } else { $null })
            combatLifecycleContract = $(if ($case.mode -eq 'CombatLifecycle') { [ordered]@{ areaWindupSeconds = 3; areaCooldownRule = 'max(configured cooldown, test windup + configured recovery + 1 second)'; ruleScope = 'Transient per-character config copies in server and both client processes; actual recovery retained, no asset or global CDO mutation'; cancellationCases = @('BeforeAuthorityHandoff', 'AfterCaughtUpAndReceivedCastingEffect'); movementInput = 'Owned PlayerController InputKey W press/release through EnhancedInput and CMC; no SetWalking or direct movement input'; movementCases = @('Join', 'Release', 'Cancel', 'ConfirmedCancel', 'StunExpiry'); minimumForwardTravelCm = 150; movementRequiredRoles = @('Server', 'Client', 'Partner') } } else { $null })
            port = $port; startedAtUtc = [DateTime]::UtcNow.ToString('o'); durationSeconds = 0; status = 'planned'
            requiredAssertions = @(Get-RequiredAssertions -Mode $case.mode); commands = @(); peers = @(); events = @(); errors = @(); motionMetrics = @(); combatTrafficMetrics = @()
        }
        $network = $profileSettings[$case.profile]
        $peerSpecs = @{}
        foreach ($role in @('Server', 'Partner', 'Client')) {
            $logPath = Join-Path $OutputDirectory "$caseId-$role.log"
            $url = if ($role -eq 'Server') { $Map } else { '/Game/UI/DSMenu' }
            $arguments = @()
            if ($BuildKind -eq 'editor-game') { $arguments += $ProjectPath }
            $arguments += @($url, '-game', '-NoSplash', '-NoSound', '-unattended', '-DDC=InstalledNoZenLocalFallback', "-abslog=$logPath", "-port=$port", "-CoopTestToken=$token", "-CoopNetTest=$($case.mode)", "-CoopTestRole=$role", "-PktLag=$($network.lag)", "-PktLagVariance=$($network.variance)", "-PktLoss=$($network.loss)")
            if ($role -eq 'Server') { $arguments += '-server' }
            $arguments += @("-CoopServerAddress=127.0.0.1:$port")
            if (($Visible -or $Offscreen) -and $role -ne 'Server') { $arguments += @('-windowed', '-ResX=960', '-ResY=540') } else { $arguments += '-NullRHI' }
            # 离屏使用项目采用的 DX12 渲染路径，窗口仍隐藏；不能与 NullRHI 的性能数据混算。
            if ($Offscreen -and $role -ne 'Server') { $arguments += @('-RenderOffscreen', '-dx12') }
            # 并发进程各用 GUID 子目录，着色器临时文件留在项目 Saved 中，不写用户目录。
            $peerMaxFps = if ($case.mode -eq 'CombatTraffic' -and $role -eq 'Server') { 30 } else { 60 }
            $arguments += @('-Multiprocess', "-ShaderWorkingDir=$(Join-Path $ProjectRoot 'Saved/ShaderWorking')", '-DDC-ForceMemoryCache', '-NoVSync', "-ExecCmds=t.MaxFPS $peerMaxFps", "-CoopTestTimeout=$TimeoutSeconds")
            $arguments += @("-CoopPlatformNetHz=$PlatformNetHz", "-CoopPlatformSyncMode=$PlatformSyncMode", "-CoopPlatformMotion=$PlatformMotion")
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
            $primaryReady = $false
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
                    if ($finished) { [void]$process.WaitForExit(30000) }
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
            $result.motionMetrics = $verdict.motionMetrics
            $result.combatTrafficMetrics = $verdict.combatTrafficMetrics
            if ($failure) { $result.errors += $failure }
            if ($result.errors.Count -gt 0 -and $result.status -ne 'unsupported') { $result.status = 'failed' }
            Save-Report
        }
    }
}
finally {
    foreach ($group in @($report.cases | Group-Object {
        if ($_.mode -in @('Combat', 'CombatTraffic', 'CombatReconnect', 'CombatLifecycle')) { "$($_.mode)/$($_.profile)" }
        else { "RideMotion/$($_.profile)/$($_.platformNetHz)Hz/$($_.platformSyncMode)/$($_.platformMotion)" }
    })) {
        $report.summaries += [ordered]@{
            group = $group.Name; attempted = $group.Count
            passed = @($group.Group | Where-Object status -eq 'passed').Count
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
    $markdown = New-Object 'System.Collections.Generic.List[string]'
    $markdown.Add('# Network validation report')
    $markdown.Add('')
    $markdown.Add("Run: $RunId; status: $($report.status); commit: $gitCommit; dirty: $($gitStatus.Count -gt 0).")
    $markdown.Add('')
    $markdown.Add('This is automated assertion evidence, not visual or manual end-to-end acceptance. Planned, failed and unsupported cases are not measured successes.')
    $markdown.Add('')
    $markdown.Add('| Case group | Passed / attempted |')
    $markdown.Add('| --- | ---: |')
    foreach ($summary in $report.summaries) { $markdown.Add("| $($summary.group) | $($summary.passed) / $($summary.attempted) |") }
    $markdown.Add('')
    $markdown.Add('See report.json for commands, configuration, fingerprints, per-assertion events, motion measurements and CombatTraffic server traffic measurements. No CPU measurements are produced.')
    if (@($report.cases | Where-Object mode -eq 'CombatTraffic').Count -gt 0) {
        $markdown.Add('CombatTraffic schedule projectile-energy-v2 fixes the test server t.MaxFPS to 30 before its 3-second warmup, with measured server FPS required within 28..34. Clients and other modes keep their 60 FPS cap. The 15-second server sample has 30 real projectile spawns and 60 energy changes (30/70), followed by both-client projectile/public-health checks. Counters are UNetDriver-accounted outbound traffic for both client connections, including overhead, ACKs and retransmissions; they are not a NIC capture or GAS-only cost. Configured projectile Hz, the server network tick limit and measured frames/second are distinct. Only passed cases below represent measured successes; compare identical profiles, map, build kind and schedule across builds; v1 and v2 samples must not be combined.')
        $markdown.Add('')
        $markdown.Add('| Case | Bytes | Packets | Seconds | B/s | Packets/s | Frames/s | Projectile Hz | Server tick limit |')
        $markdown.Add('| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |')
        foreach ($trafficCase in @($report.cases | Where-Object { $_.mode -eq 'CombatTraffic' -and $_.status -eq 'passed' })) {
            $traffic = $trafficCase.combatTrafficMetrics[0].metrics
            $markdown.Add(('| {0} | {1} | {2} | {3:F3} | {4:F2} | {5:F2} | {6:F2} | {7} | {8} |' -f $trafficCase.id, $traffic.outBytes, $traffic.outPackets, $traffic.elapsedSeconds, $traffic.bytesPerSecond, $traffic.packetsPerSecond, $traffic.framesPerSecond, $traffic.projectileNetHz, $traffic.serverNetTickRate))
        }
    }
    if (@($report.cases | Where-Object mode -eq 'Combat').Count -gt 0) { $markdown.Add('Combat checks authoritative damage/healing/death, instant skills and Area predicting real cost/cooldown effects before the local call returns, PredictionKey catch-up without double cost, and rejection after server-only cooldown/resource/target/death divergence. Area adds cooldown/resource rejection with keyed casting-effect rollback and a legal asynchronous release/recovery retry. Rejection fixtures briefly delay pawn property replication while preserving the real GAS RPC path; the owner must observe real predicted effects, rejection and local rollback before the authoritative fixture is released. Each condition is followed by a legal double-click retry; death retries use an intentional exit and a new session, not personal respawn. Both clients independently verify the replicated state. It does not certify animation, rendered projectile quality or full AI navigation.') }
    if (@($report.cases | Where-Object mode -eq 'CombatReconnect').Count -gt 0) { $markdown.Add('CombatReconnect closes the real primary connection and uses the engine network-failure entry to exercise GI recovery and the owned-controller admission handshake. A new controller/pawn must restore health, owner energy, all four elapsed cooldowns, remaining stun, death/collision and position while the partner controller/pawn and shared progress remain unchanged. Both connections independently observe restored control; unfinished casting is not resumed. Ticket active/invalid/rotation/lost-receipt boundaries are checked at the server admission entry; ticket values are never logged. This is not a fresh-session or same-pawn respawn test.') }
    if (@($report.cases | Where-Object mode -eq 'CombatLifecycle').Count -gt 0) { $markdown.Add('CombatLifecycle starts the primary connection after a server enemy was already stunned. Only this mode uses transient per-character config copies in all three processes with Area.Windup=3 seconds and Area.Cooldown=max(configured cooldown, test windup+configured recovery+1 second), retaining the actual recovery and leaving assets, the global CDO and all other rules unchanged. It checks local Area prediction keys and keyed cost/cooldown effects, one server settlement, cancellation both before confirmation and after key catch-up plus receipt of the same-key authoritative Casting effect, and server stun interruption. Cancellation must immediately clear owner casting without refunding its one cost or deleting cooldown; the server checks retained cooldown and no damage after the original release deadline. Both clients independently observe final casting/health/resource state. After admission, normal release, both cancellations and actual stun expiry, each owner presses/releases W through its real PlayerController/EnhancedInput mapping: both local replicas and the authoritative pawns must move forward at least 150cm without the test changing movement mode or teleporting during input. The aiming floor is replicated. This is not rendered animation/VFX/UI acceptance, arbitrary target-data prediction, or public-cloud reachability.') }
    if (@($report.cases | Where-Object mode -eq 'RideMotion').Count -gt 0) { $markdown.Add('RideMotion phase measurements are preserved as complete events in each case''s motionMetrics array in report.json, including role, phase, sample count, base/falling observations and correction distances. Missing or non-finite measurements fail the case; the numerical checks are not a replacement for visual acceptance.') }
    if (@($report.cases | Where-Object mode -eq 'RideMotion').Count -gt 0 -and -not $PlanOnly) { $markdown.Add('RideMotion client Jump timeline: timeline-<case id>.svg, plus -samples.csv, -corrections.csv, -move-pairs.csv and -all-moves.csv. Moves are joined by CMC timestamp; server/client World clocks are not compared. This is not a packet trace or proof of visual smoothness.') }
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'summary.md'), ($markdown -join "`r`n"), $Utf8)
}
Write-Output "Report: $(Join-Path $OutputDirectory 'report.json')"
if ($PlanOnly) { Write-Output 'Plan only: no Unreal processes started and no measurements claimed.' }
elseif ($report.status -ne 'passed') { throw 'One or more network validation cases failed, were unsupported, or did not complete. See report.json.' }
