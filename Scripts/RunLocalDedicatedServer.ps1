<#
本机独立 DS + 两个客户端。默认验证完整撤离关卡；-Play 打开可操作窗口。
使用 Editor 的 -server，不等于已经构建独立 Server.exe。只清理本脚本启动的进程。
#>
[CmdletBinding()]
param(
    [string]$EditorPath = 'E:/program/ue554/UE_5.5/Engine/Binaries/Win64/UnrealEditor.exe',
    [string]$Map = '/Game/Combat/Maps/CoopEscape',
    [ValidateRange(0,65535)][int]$Port = 0,
    [ValidateRange(30,600)][int]$TimeoutSeconds = 300,
    [ValidateSet('Normal','Moderate','Harsh')][string]$Profile = 'Normal',
    [ValidateRange(0,10)][int]$EnemyCount = 0,
    [switch]$Play,
    [switch]$SkillDemo,
    [switch]$FromMenu,
    [switch]$PlanOnly
)
$ErrorActionPreference = 'Stop'
if ($FromMenu -and -not $Play) { throw '-FromMenu requires -Play.' }
if ($SkillDemo -and (-not $Play -or $Map -ne '/Game/Combat/Maps/CombatTest' -or $FromMenu -or $EnemyCount -gt 0)) {
    throw '-SkillDemo requires -Play -Map /Game/Combat/Maps/CombatTest, without FromMenu or EnemyCount.'
}
if ($EnemyCount -gt 0 -and (-not $Play -or $Map -ne '/Game/Combat/Maps/CombatTest')) {
    throw '-EnemyCount is a temporary manual CombatTest override; use -Play -Map /Game/Combat/Maps/CombatTest.'
}
if (-not $Play) {
    & (Join-Path $PSScriptRoot 'RunMultiplayerNetworkTests.ps1') -Scenario CoopEscape -Profiles @($Profile) -Repeat 1 -EditorPath $EditorPath -Map $Map -ListenPort $Port -TimeoutSeconds $TimeoutSeconds -PlanOnly:$PlanOnly
    return
}

# 以下仅负责交互式启动，不维护第二套测试断言、报告或通过条件。
$projectRoot = Split-Path $PSScriptRoot -Parent
$projectPath = Join-Path $projectRoot 'multiplayer.uproject'
if (-not (Test-Path -LiteralPath $EditorPath)) { throw "Editor not found: $EditorPath" }
if ($Port -eq 0) {
    $socket = [Net.Sockets.UdpClient]::new(0)
    try { $Port = $socket.Client.LocalEndPoint.Port } finally { $socket.Dispose() }
}
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)
$outputPath = Join-Path $projectRoot "Saved/DedicatedServer/$runId"
$network = switch ($Profile) {
    Normal { @(0,0,0) }
    Moderate { @(100,20,2) }
    Harsh { @(200,50,5) }
}
$peers = foreach ($name in @('Server','Client1','Client2')) {
    $server = $name -eq 'Server'
    # 菜单模式只打开本地连接界面；玩家通过原有 GameInstance 流程加入，不绕过地址输入和握手。
    $url = if ($server) { $Map } elseif ($FromMenu) { '/Game/UI/DSMenu' } else { "127.0.0.1:$Port" }
    $commands = 't.MaxFPS 60'
    if ($SkillDemo) {
        $commands += ',AbilitySystem.IgnoreCosts 1,AbilitySystem.IgnoreCooldowns 1'
        if ($server) { $commands += ',setnopec multiplayerEnemySpawner EnemyClass None,getall multiplayerEnemySpawner EnemyClass' }
    }
    if ($server -and $EnemyCount -gt 0) {
        # 首个玩家接纳前调整本次运行的出生器，不保存地图，也不改变正式波次参数。
        $commands += ",setnopec multiplayerEnemySpawner EnemyCount $EnemyCount,getall multiplayerEnemySpawner EnemyCount"
    }
    $arguments = @($projectPath, $url, '-game', '-nosplash', '-nosound', '-NoVSync', '-DDC=InstalledNoZenLocalFallback',
        '-DDC-ForceMemoryCache', "-abslog=$(Join-Path $outputPath "$name.log")", "-port=$Port", "-ExecCmds=$commands",
        "-PktLag=$($network[0])", "-PktLagVariance=$($network[1])", "-PktLoss=$($network[2])")
    if ($SkillDemo) { $arguments += '-CoopSkillDemo' }
    if ($server) { $arguments += @('-server','-NullRHI','-unattended') }
    else {
        $windowX = if ($name -eq 'Client1') { 20 } else { 1000 }
        $arguments += @('-windowed','-ResX=960','-ResY=540',"-WinX=$windowX",'-WinY=60')
    }
    [pscustomobject]@{ name=$name; arguments=$arguments; process=$null }
}
if ($PlanOnly) {
    foreach ($peer in $peers) { Write-Output "$($peer.name): $EditorPath $($peer.arguments -join ' ')" }
    return
}

function Read-ServerLog {
    $path = Join-Path $outputPath 'Server.log'
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $stream = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    $reader = [IO.StreamReader]::new($stream)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Start-Peer($Peer) {
    $line = ($Peer.arguments | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' '
    $style = if ($Peer.name -eq 'Server') { 'Hidden' } else { 'Normal' }
    $Peer.process = Start-Process -FilePath $EditorPath -ArgumentList $line -WorkingDirectory $projectRoot -WindowStyle $style -PassThru
    Write-Host "Started $($Peer.name), PID $($Peer.process.Id)"
}

[void](New-Item -ItemType Directory -Path $outputPath)
$watch = [Diagnostics.Stopwatch]::StartNew()
try {
    Start-Peer $peers[0]
    do {
        if ($peers[0].process.HasExited) { throw 'Dedicated server exited before ready' }
        $ready = (Read-ServerLog) -match 'GameNetDriver.*listening on port'
        if ($SkillDemo) { $ready = $ready -and (Read-ServerLog) -match '\.EnemyClass = None\b' }
        if ($EnemyCount -gt 0) { $ready = $ready -and (Read-ServerLog) -match ("\.EnemyCount = $EnemyCount\b") }
        if ($watch.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw 'Dedicated server ready timeout' }
        if (-not $ready) { Start-Sleep -Milliseconds 250 }
    } until ($ready)
    Start-Peer $peers[1]
    Start-Peer $peers[2]
    Write-Host "DS: 127.0.0.1:$Port | logs: $outputPath"
    if ($SkillDemo) { Write-Host 'SKILL DEMO: no AI; all five skills unlocked; no costs/cooldowns; full-health self heal allowed. Normal action/cancellation rules remain.' }
    if ($FromMenu) { Write-Host "Both clients start at the connection menu. Connect each one to 127.0.0.1:$Port." }
    Write-Host 'Close both client windows or press Ctrl+C here to end this local session.'
    while (-not ($peers[1].process.HasExited -and $peers[2].process.HasExited)) {
        if ($peers[0].process.HasExited) { throw 'Dedicated server exited unexpectedly' }
        Start-Sleep -Milliseconds 500
    }
}
finally {
    foreach ($peer in $peers) {
        if ($peer.process -and -not $peer.process.HasExited) {
            $peer.process.Kill()
            [void]$peer.process.WaitForExit(5000)
        }
    }
}
Write-Host "Manual session ended; logs: $outputPath"
