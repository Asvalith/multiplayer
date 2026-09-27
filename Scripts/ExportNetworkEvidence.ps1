[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ReportPath,
    [Parameter(Mandatory)][string]$OutputPath,
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSourceSha256
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 只抽取已结束报告，不启动 UE、不执行报告里的命令，也不重新判定通过。
$sourcePath = (Resolve-Path -LiteralPath $ReportPath).Path
$destination = [IO.Path]::GetFullPath($OutputPath)
if ($sourcePath -eq $destination -or (Test-Path -LiteralPath $destination)) {
    throw '输出必须是新文件，不能覆盖原报告或既有证据。'
}
$sourceBytes = [IO.File]::ReadAllBytes($sourcePath)
$sha = [Security.Cryptography.SHA256]::Create()
try { $sourceHash = ([BitConverter]::ToString($sha.ComputeHash($sourceBytes))).Replace('-', '').ToLowerInvariant() }
finally { $sha.Dispose() }
if ($ExpectedSourceSha256 -and $sourceHash -ine $ExpectedSourceSha256) {
    throw '原报告 SHA-256 与预期不符，停止导出。'
}
$report = ([Text.Encoding]::UTF8.GetString($sourceBytes).TrimStart([char]0xFEFF)) | ConvertFrom-Json
if ($report.schemaVersion -ne 1 -or $report.planOnly -ne $false -or $report.status -notin @('passed', 'failed')) {
    throw '仅接受 schemaVersion=1 且已结束的实测报告，不导出 running/planned 计划。'
}
if (@($report.cases | Where-Object { $_.status -notin @('passed', 'failed', 'unsupported') }).Count) {
    throw '报告仍有未结束案例，停止导出。'
}

function Select-PublicFields {
    param($Object, [string[]]$Names)
    if ($null -eq $Object) { return $null }
    $result = [ordered]@{}
    foreach ($name in $Names) {
        if ($null -ne $Object.PSObject.Properties[$name]) { $result[$name] = $Object.$name }
    }
    return $result
}

# 白名单摘录之外再递归脱敏，尤其处理断言 detail/error 内嵌的 CSV 或包文件路径。
# 含绝对路径的文本整段隐藏，避免带空格的路径只删掉前半截。
$machineName = [string]$report.environment.machine
function ConvertTo-PublicValue {
    param($Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [string]) {
        if ($Value -match '(?<![A-Za-z0-9])[A-Za-z]:[\\/]|\\\\[^\\/\s]+[\\/]|/(Users|home|tmp)/') {
            return '[已隐藏含本机绝对路径的文本；原文见原报告]'
        }
        # Null 子系统的在线标识会夹带大小写不同的机器名；网络地址也无需公开。
        $Value = [regex]::Replace($Value, 'UniqueId:\s*NULL:[^\s,;]+', 'UniqueId: NULL:[redacted]')
        $Value = [regex]::Replace($Value, 'RemoteAddr:\s*[^\s,;]+', 'RemoteAddr: [test-peer]')
        if ($machineName) {
            return [regex]::Replace($Value, '(?<!\w)' + [regex]::Escape($machineName) + '(?!\w)', '[machine]')
        }
        return $Value
    }
    if ($Value -is [datetime]) { return $Value.ToUniversalTime().ToString('o') }
    if ($Value -is [System.Collections.IDictionary]) {
        $result = [ordered]@{}
        foreach ($key in $Value.Keys) {
            if ($key -in @('path', 'csvPath', 'machine', 'commands', 'rendered', 'peers', 'gitStatus', 'sourceManifest')) { continue }
            $result[$key] = ConvertTo-PublicValue $Value[$key]
        }
        return $result
    }
    if ($Value -is [array]) {
        $items = @($Value | ForEach-Object { ConvertTo-PublicValue $_ })
        return ,$items
    }
    if ($Value -is [pscustomobject]) {
        $result = [ordered]@{}
        foreach ($property in $Value.PSObject.Properties) { $result[$property.Name] = $property.Value }
        return ConvertTo-PublicValue $result
    }
    return $Value
}

function Get-DerivedFrameTiming {
    param($Case)
    $result = [ordered]@{ value = $null; warning = $null }
    if ($Case.mode -ne 'Scale') { return $result }
    if ($null -eq $Case.PSObject.Properties['csvPath'] -or $null -eq $Case.PSObject.Properties['csvSha256'] -or
        -not $Case.csvPath -or -not $Case.csvSha256 -or -not (Test-Path -LiteralPath $Case.csvPath -PathType Leaf)) {
        $result.warning = '服务器 CSV 或原始 CSV 指纹缺失，未生成派生帧节奏。'
        return $result
    }
    try {
        $bytes = [IO.File]::ReadAllBytes($Case.csvPath)
        $hasher = [Security.Cryptography.SHA256]::Create()
        try { $hash = ([BitConverter]::ToString($hasher.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
        finally { $hasher.Dispose() }
        if ($hash -ine $Case.csvSha256) { throw 'CSV 指纹不符' }
        $lines = ([Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)) -split '\r?\n'
        $header = -1
        for ($index = 0; $index -lt $lines.Count; $index++) {
            if (($lines[$index] -split ',') -contains 'FrameTime') { $header = $index; break }
        }
        if ($header -lt 0) { throw 'CSV 缺少 FrameTime' }
        $rows = @(($lines[$header..($lines.Count - 1)] -join "`n") | ConvertFrom-Csv)
        $values = @($rows | ForEach-Object {
            $number = 0.0
            if ([double]::TryParse([string]$_.FrameTime, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -and
                -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number) -and $number -gt 0) { $number }
        } | Sort-Object)
        if ($values.Count -eq 0) { throw 'FrameTime 没有有限正数样本' }
        $mean = ($values | Measure-Object -Average).Average
        $p95Index = [Math]::Max(0, [int][Math]::Ceiling($values.Count * 0.95) - 1)
        $result.value = [ordered]@{
            sourceCsvSha256 = $hash; sourceColumn = 'FrameTime'; unit = 'ms'
            sampleCount = $values.Count; meanMs = [Math]::Round($mean, 6)
            p95Ms = [Math]::Round($values[$p95Index], 6); p95Method = 'nearest-rank'
            estimatedFPS = [Math]::Round(1000.0 / $mean, 6)
            interpretation = '由服务器 CSV 的有限正数 FrameTime 派生；FPS=1000/meanMs，不代表游戏线程纯计算耗时，也不把 60 FPS 上限视为实测帧率。'
        }
    } catch {
        $result.warning = '服务器 CSV 不可读取、指纹不符或 FrameTime 无有效样本；派生帧节奏保持 null，原案例状态未改写。'
    }
    return $result
}

$cases = @($report.cases | ForEach-Object {
    $case = Select-PublicFields $_ @('id', 'mode', 'matrix', 'profile', 'iteration', 'staticCount', 'movingCount', 'optimization', 'platformNetHz', 'platformSyncMode', 'warmupSeconds', 'sampleSeconds', 'samplingDurationSource', 'outageSeconds', 'startedAtUtc', 'durationSeconds', 'status', 'requiredAssertions', 'errors', 'metrics', 'motionMetrics', 'cpuMetrics', 'csvSha256', 'measurementWarnings')
    # ASSERT、DONE 和 FAIL 原样取值；不从服务器断言推断客户端已通过。
    $case['events'] = @($_.events | Where-Object { $_.status -in @('ASSERT', 'DONE', 'FAIL', 'UNSUPPORTED') } | ForEach-Object {
        Select-PublicFields $_ @('mode', 'role', 'status', 'phase', 'assertion', 'passed', 'detail', 'elapsed')
    })
    if ($_.mode -eq 'Scale') {
        $timing = Get-DerivedFrameTiming $_
        $case['derivedFrameTiming'] = $timing.value
        $case['derivedFrameTimingWarning'] = $timing.warning
    }
    $case
})
$excerpt = [ordered]@{
    schemaVersion = 1
    artifactKind = 'sanitized-network-validation-excerpt'
    description = '自动脱敏摘录，不是原始完整报告；状态和指标直接来自原报告，未补写通过结果。'
    extractedAtUtc = [DateTime]::UtcNow.ToString('o')
    sourceReport = [ordered]@{
        fileName = [IO.Path]::GetFileName($sourcePath)
        sha256 = $sourceHash
        runId = $report.runId
        generatedAtUtc = $report.generatedAtUtc
        status = $report.status
        omitted = @('机器名、本机绝对路径', '启动命令与原始日志', 'gitStatus 与源码文件清单（含工具缓存路径）', 'STAGE/READY/RECEIPT 等过程事件')
    }
    environment = Select-PublicFields $report.environment @('engineAssociation', 'executableVersion', 'executableSha256', 'projectModule', 'buildKind', 'map', 'nullRhi', 'topology', 'visualValidation', 'runtimeGameplayConfig')
    provenance = Select-PublicFields $report.provenance @('gitCommit', 'dirty', 'sourceFingerprintSha256', 'gameplayConfigSha256')
    measurementInterpretation = [ordered]@{
        bandwidth = 'OutTotalBytes/OutTotalPackets 是引擎发送计数。UE 5.5 字节计数由发送缓冲字节数加配置的 PacketOverhead 累计，非网卡抓包、非物理链路流量，也非已成功到达客户端的字节数。B/s 为该计数差除以采样秒数。'
        frameTiming = 'derivedFrameTiming 仅在原服务器 CSV 的 SHA-256 校验通过后计算；estimatedFPS=1000/FrameTime均值，不把 60 FPS 上限当成实际帧率。'
        gameThread = 'GameThreadTime 全零表示该运行配置下计数不可用于 CPU 成本结论；原始零值和摘要保留作追溯，不代表 CPU 开销为零。'
        errata = @('部分旧二进制 metrics.counterSource 中的 wire packets, not pre-compression bytes 描述过强；摘录保留原标签和计数，解释以本节为准。')
    }
    scope = @($report.scope) + @(
        '案例通过仅指原报告中的具体自动断言，不表示人工完整通关、全部蓝图配置或画面验收。',
        'Flow 使用真实 Overlap，但角色由测试放置，并包含合成机关；ClientRide 不证明视觉平滑，VictoryWidget 入 Viewport 不证明按钮点击和像素正确。',
        'SessionRetry 和首次重开失败使用开发注入；Reconnect 使用进程内真实断包和缩短的连接超时，不是物理网络故障穷举。',
        '同机双进程会竞争 CPU；NullRHI 不测渲染。合成对象数量不等于玩家容量，100 Hz 设置不等于实测发送频率。',
        '带宽是服务器总出站；CPU 缺失或全零需保留限制。正式对照每组至少三次，单次成功不等于正式性能结论。',
        'runtimeGameplayConfig.loadedByRuntimeVerified=false 时只证明磁盘文件指纹，不能据此声称运行时已加载该配置。',
        'RideMotion 的 motionMetrics 按角色与阶段保留实际采样；平台逐帧位移、角色相对位移和历史 Move 校正误差不是同一指标。基座变化的校正另计，Host 校正字段不适用；动作通过不代表视觉平滑。',
        'projectModule.loadedByRuntimeVerified=false 时只记录磁盘上的项目模块指纹，未在运行进程内验证加载模块的哈希。'
    )
    cases = $cases
    summaries = @($report.summaries)
}
$json = (ConvertTo-PublicValue $excerpt) | ConvertTo-Json -Depth 30
# 再次确认原始文件未在导出期间被写入；指纹校验失败时不生成摘录。
if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash -ine $sourceHash) {
    throw '原报告在导出期间发生变化，停止导出。'
}
$directory = [IO.Path]::GetDirectoryName($destination)
[void][IO.Directory]::CreateDirectory($directory)
$stream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
$writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
try { $writer.WriteLine($json) } finally { $writer.Dispose(); $stream.Dispose() }
Write-Output "Exported $($cases.Count) original cases; source SHA-256: $sourceHash"
Write-Output "Excerpt: $destination"
