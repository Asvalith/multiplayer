[CmdletBinding()]
param([switch]$Plan, [switch]$SelfTest, [switch]$Replace)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$mainPath = Join-Path $repoRoot 'Docs/Study/UE网络同步技术博客.md'
$appendixPath = Join-Path $repoRoot 'Docs/Reports/2026-09-27/Appendix.md'
$outputPath = Join-Path $repoRoot 'Docs/Exports/UE网络同步技术博客_Notion完整版.md'
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$linkPattern = '(?<!!)\[(?<label>[^\]\r\n]+)\]\((?<target><[^>\r\n]+>|[^()\s]+)(?:\s+"[^"]*")?\)'

function Convert-Link([System.Text.RegularExpressions.Match]$Match, [string]$BaseDirectory) {
    $label = $Match.Groups['label'].Value
    $target = $Match.Groups['target'].Value.Trim('<', '>')
    # 导入页只保留资料名称；网址和本地跳转都不进入交付正文。
    if ($target -match '^(https?://|mailto:)') { return $label }
    if ($target.StartsWith('#')) { return $label }
    $target = [Uri]::UnescapeDataString($target).Replace('\', '/')
    if ($target -match '^[A-Za-z]:/.+?/(?<engine>Engine/.*)$') {
        return $label + '（引擎源码定位：`' + $Matches['engine'] + '`）'
    }
    if ($target -match '^([A-Za-z]:/|/|file:|\\\\)') { throw "未识别的本机链接：$target" }
    $pathPart = ($target -split '#', 2)[0] -replace ':\d+(?::\d+)?$', ''
    $suffix = $target.Substring($pathPart.Length)
    $resolved = [IO.Path]::GetFullPath((Join-Path $BaseDirectory $pathPart))
    $prefix = $repoRoot + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "仓库链接越出工程范围：$target"
    }
    $relative = $resolved.Substring($prefix.Length).Replace('\', '/') + $suffix
    $hint = if ($resolved -eq $appendixPath) { '见文末附录；仓库路径：' } else { '仓库路径：' }
    return $label + '（' + $hint + '`' + $relative + '`）'
}

function Convert-Markdown([string]$Text, [string]$BaseDirectory, [int]$HeadingOffset = 0, [switch]$Inspect) {
    $result = [Text.StringBuilder]::new()
    $prose = [Text.StringBuilder]::new()
    $codes = [Collections.Generic.List[string]]::new()
    $code = [Text.StringBuilder]::new()
    $fence = ''; $fenceLength = 0
    # 保留原始换行，尤其不能改动代码围栏内的示例。
    foreach ($item in [regex]::Matches($Text, '(?m)^.*(?:\n|$)')) {
        $raw = $item.Value
        if (-not $raw.Length) { continue }
        $line = $raw -replace '\r?\n$', ''
        $ending = $raw.Substring($line.Length)
        if ($fence) {
            [void]$result.Append($raw); [void]$code.Append($raw)
            if ($line -match ('^ {0,3}' + [regex]::Escape($fence) + '{' + $fenceLength + ',}\s*$')) {
                $codes.Add($code.ToString()); [void]$code.Clear(); $fence = ''
            }
            continue
        }
        if ($line -match '^ {0,3}(`{3,}|~{3,})(.*)$') {
            $fence = $Matches[1].Substring(0, 1); $fenceLength = $Matches[1].Length
            [void]$result.Append($raw); [void]$code.Append($raw)
            continue
        }
        if (-not $Inspect) {
            if ($line -match '^\s*#?\s*<a\b[^>]*>\s*</a>\s*$') { continue }
            $line = [regex]::Replace($line, $linkPattern, [Text.RegularExpressions.MatchEvaluator]{
                param($match) Convert-Link $match $BaseDirectory
            })
            if ($line -match '^(#{1,6})\s+(.+?)\s*#*\s*$') {
                $level = $Matches[1].Length + $HeadingOffset; $title = $Matches[2]
                $line = if ($level -ge 4) { '**' + $title + '**' } else { ('#' * $level) + ' ' + $title }
            }
        }
        [void]$result.Append($line).Append($ending)
        [void]$prose.Append($line).Append($ending)
    }
    if ($fence) { throw '代码围栏未闭合，停止导出。' }
    [pscustomobject]@{ Text = $result.ToString(); Prose = $prose.ToString(); Codes = $codes.ToArray() }
}

if ($SelfTest) {
    $sample = "#### Small`r`n[chapter](#x) [code](../../Source/test.cpp:12) [web](https://example.org/x)`r`n" +
        "``````text`r`n#### Keep [link](E:/private/file)`r`n```````r`n" + '<a id="x"></a>'
    $check = Convert-Markdown $sample (Split-Path $mainPath)
    $original = Convert-Markdown $sample '' -Inspect
    if ($check.Text -notmatch '\*\*Small\*\*' -or $check.Text -notmatch 'Source/test.cpp:12' -or
        $check.Text -match '\[web\]\(' -or $check.Text -notmatch '\bweb\b' -or $check.Prose -match '<a |\]\(#' -or
        $check.Codes.Count -ne 1 -or $check.Codes[0] -cne $original.Codes[0]) { throw 'SelfTest failed.' }
    Write-Host 'SelfTest passed; no files written.'
    return
}

if (-not $Plan -and -not $Replace -and (Test-Path -LiteralPath $outputPath)) { throw '输出文件已存在；只有显式 -Replace 才更新生成的交付版。' }
$mainText = [IO.File]::ReadAllText($mainPath, $utf8).TrimStart([char]0xFEFF)
$appendixText = [IO.File]::ReadAllText($appendixPath, $utf8).TrimStart([char]0xFEFF)
$main = Convert-Markdown $mainText (Split-Path $mainPath)
$appendix = Convert-Markdown $appendixText (Split-Path $appendixPath) 1
$firstBreak = $main.Text.IndexOf("`n")
if ($firstBreak -lt 0 -or $main.Text -notmatch '^# [^\r\n]+') { throw '主文缺少首行标题。' }
$intro = @'

> 导入说明：这是本地 Markdown 文件，尚未上传或线上发布。五章正文与历史数据附录保留在同一文件。Notion 桌面端或网页端：设置 → 导入 → 文本和 Markdown，选择本文件。普通 Markdown 锚点不受支持，导入后可插入 Notion 目录区块。
>
> 所有引用保留为资料名称、RFC 编号或代码路径，没有可点击网址或仓库跳转。文末附录保留构建 A/B 的历史数据边界，不替代正文说明的新实验记录。原始数据包未随文件导出。
>
> 文中关键机理有可直接导入的文字流程，实验有完整数值表；对应的独立 PNG 位于 `figures` 文件夹，可按图表编号手动插入 Notion。
>
> 如需核对导入步骤，在 Notion 帮助中心搜索“将数据导入 Notion”。

'@
$merged = $main.Text.Substring(0, $firstBreak + 1) + $intro + $main.Text.Substring($firstBreak + 1) +
    "`n`n---`n`n" + $appendix.Text
$verified = Convert-Markdown $merged '' -Inspect
$sourceQ = [regex]::Matches($mainText + "`n" + $appendixText, '(?m)^>\s*\*\*Q[：:]').Count
$outputQ = [regex]::Matches($verified.Prose, '(?m)^>\s*\*\*Q[：:]').Count
foreach ($chapter in @('一', '二', '三', '四', '五')) {
    if ([regex]::Matches($verified.Prose, '(?m)^## 第' + $chapter + '章[：:]').Count -ne 1) { throw "章节缺失或重复：$chapter" }
}
if ([regex]::Matches($verified.Prose, '(?m)^# ').Count -ne 1 -or $sourceQ -ne $outputQ) { throw '标题或 Q 引用块数量不匹配。' }
if ($verified.Prose -match '(?im)\bTODO\b|\bTBD\b|待补充|待完善|占位符|^#{4,6}\s|<a\b|\]\(#') { throw '发现占位、深层标题或未转换锚点。' }
foreach ($link in [regex]::Matches($verified.Prose, $linkPattern)) { throw "残留可点击链接：$($link.Value)" }
$sourceCodes = @($main.Codes) + @($appendix.Codes)
if ($sourceCodes.Count -ne $verified.Codes.Count) { throw '代码块数量不匹配。' }
for ($i = 0; $i -lt $sourceCodes.Count; $i++) {
    if ($sourceCodes[$i] -cne $verified.Codes[$i]) { throw "代码块内容发生变化：$i" }
}
$bytes = $utf8.GetBytes($merged)
if (-not $Plan) {
    [void][IO.Directory]::CreateDirectory((Split-Path $outputPath))
    $mode = if ($Replace) { [IO.FileMode]::Create } else { [IO.FileMode]::CreateNew }
    $stream = [IO.File]::Open($outputPath, $mode, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}
Write-Host ('{0}: 5 chapters, {1} Q blocks, {2} unchanged code blocks, {3} UTF-8 bytes.' -f
    $(if ($Plan) { 'Plan passed; no files written' } else { 'Exported ' + $outputPath }), $outputQ, $sourceCodes.Count, $bytes.Length)
